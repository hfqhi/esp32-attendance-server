import csv
import io
import os
import sqlite3
import time
import urllib.request
from datetime import datetime

import numpy as np
from flask import Flask, Response, jsonify, render_template, request, send_from_directory

from database import (
    get_active_schedule,
    get_db,
    get_ended_classes_with_missing_students,
    init_db,
    validate_schedule_input,
)
from face_engine import (
    extract_face_feature,
    identify_offline_face,
    save_reference_portrait,
    verify_and_check_liveness,
)

app = Flask(__name__)
init_db()

# Tracks live ESP32-CAM connection state
esp32_node = {
    "ip": None,
    "last_seen": 0.0,
}

def get_esp32_status():
    online = bool(esp32_node["ip"] and (time.time() - esp32_node["last_seen"] < 15.0))
    return {
        "online": online,
        "ip": esp32_node["ip"] if online else None,
        "stream_url": f"http://{esp32_node['ip']}:81/stream" if online else None,
        "capture_url": f"http://{esp32_node['ip']}/capture" if online else None,
    }

@app.route("/")
def index():
    return render_template("index.html")

# --- ESP32-CAM DEVICE ENDPOINTS ---
@app.route("/api/esp32/register", methods=["POST"])
def register_esp32():
    data = request.json or {}
    ip_addr = data.get("ip") or request.remote_addr
    esp32_node["ip"] = ip_addr
    esp32_node["last_seen"] = time.time()
    return jsonify({"success": True, "ip": ip_addr, "server_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S")})

@app.route("/api/esp32/proxy_capture", methods=["GET"])
def proxy_esp32_capture():
    """Fetches a single JPEG from the ESP32-CAM to avoid browser canvas CORS blocks."""
    status = get_esp32_status()
    if not status["online"]:
        return jsonify({"success": False, "message": "ESP32-CAM is offline."}), 503
    try:
        with urllib.request.urlopen(status["capture_url"], timeout=3) as resp:
            img_bytes = resp.read()
        return Response(img_bytes, mimetype="image/jpeg")
    except Exception as exc:
        return jsonify({"success": False, "message": str(exc)}), 502

@app.route("/api/esp32/enrolled_portraits", methods=["GET"])
def list_enrolled_portraits():
    """Lists stored reference JPEG portraits so ESP32-CAM can mirror them onto its SD card."""
    files = [
        f for f in os.listdir("enrolled_faces")
        if f.lower().endswith(".jpg")
    ]
    return jsonify({"portraits": sorted(files)})

@app.route("/api/esp32/portraits/<path:filename>", methods=["GET"])
def download_portrait(filename):
    return send_from_directory("enrolled_faces", filename)

@app.route("/api/esp32/sync_offline", methods=["POST"])
def sync_offline_capture():
    """Processes an offline-queued JPEG from the ESP32-CAM SD card."""
    esp32_node["last_seen"] = time.time()
    raw_bytes = request.data
    orig_date = request.headers.get("X-Capture-Date", "").strip()
    orig_time = request.headers.get("X-Capture-Time", "").strip()

    student = identify_offline_face(raw_bytes)
    if not student:
        return jsonify({"status": "UNKNOWN", "student_name": "Unrecognized", "message": "No enrolled face matched"})

    subject_code, subject, att_status = get_active_schedule()
    if not subject:
        subject_code = "OFFLINE"
        subject = "Offline Sync"
        att_status = "PRESENT"

    now_dt = datetime.now()
    date_str = orig_date if len(orig_date) == 10 else now_dt.strftime("%Y-%m-%d")
    time_str = orig_time if orig_time else now_dt.strftime("%I:%M:%S %p")

    conn = get_db()
    try:
        conn.execute("""
            INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (student["student_id"], student["full_name"], subject_code or "", subject, att_status, date_str, time_str))
        conn.commit()
    except sqlite3.IntegrityError:
        pass
    finally:
        conn.close()

    return jsonify({
        "status": "LOGGED",
        "student_id": student["student_id"],
        "student_name": student["full_name"],
        "attendance_status": att_status,
        "subject_code": subject_code,
        "message": f"Synced {student['full_name']}",
    })

# --- DASHBOARD DATA & CRUD ---
@app.route("/api/dashboard_data", methods=["GET"])
def dashboard_data():
    conn = get_db()
    students = [dict(r) for r in conn.execute("SELECT * FROM students ORDER BY full_name").fetchall()]
    schedules = [dict(r) for r in conn.execute("""
        SELECT * FROM schedules
        ORDER BY CASE day_of_week
            WHEN 'Monday' THEN 1
            WHEN 'Tuesday' THEN 2
            WHEN 'Wednesday' THEN 3
            WHEN 'Thursday' THEN 4
            WHEN 'Friday' THEN 5
            WHEN 'Saturday' THEN 6
            WHEN 'Sunday' THEN 7
            ELSE 8
        END, start_time ASC
    """).fetchall()]
    logs = [dict(r) for r in conn.execute("SELECT * FROM attendance_logs ORDER BY date_str DESC, id DESC").fetchall()]
    conn.close()

    active_code, active_subject, _ = get_active_schedule()
    today_name = datetime.now().strftime("%A")
    active_label = f"{active_code} • {active_subject}" if active_subject else f"No Active Class ({today_name})"

    return jsonify({
        "students": students,
        "schedules": schedules,
        "logs": logs,
        "active_subject": active_label,
        "pending_absences": get_ended_classes_with_missing_students(),
        "esp32": get_esp32_status(),
    })

@app.route("/api/enroll", methods=["POST"])
def enroll_student():
    data = request.json
    student_id = data.get("student_id", "").strip()
    full_name = " ".join(data.get("full_name", "").split())
    section = data.get("section", "CPE-3B").strip() or "CPE-3B"
    image_data = data.get("image", "")

    if not student_id or not full_name or not image_data:
        return jsonify({"success": False, "message": "Student ID, Name, and Photo are required."}), 400

    conn = get_db()
    if conn.execute("SELECT 1 FROM students WHERE LOWER(student_id) = LOWER(?)", (student_id,)).fetchone():
        conn.close()
        return jsonify({"success": False, "message": f"Student ID '{student_id}' is already registered."}), 409

    if conn.execute("SELECT 1 FROM students WHERE LOWER(full_name) = LOWER(?)", (full_name,)).fetchone():
        conn.close()
        return jsonify({"success": False, "message": f"'{full_name}' is already enrolled."}), 409
    conn.close()

    feature, err = extract_face_feature(image_data)
    if err:
        return jsonify({"success": False, "message": err}), 400

    safe_id = "".join(c for c in student_id if c.isalnum() or c in ("-", "_"))
    emb_filename = f"{safe_id}.npy"
    np.save(os.path.join("enrolled_faces", emb_filename), feature)
    save_reference_portrait(image_data, student_id, full_name)

    conn = get_db()
    conn.execute("""
        INSERT INTO students (student_id, full_name, section, embedding_file, created_at)
        VALUES (?, ?, ?, ?, ?)
    """, (student_id, full_name, section, emb_filename, datetime.now().strftime("%Y-%m-%d %I:%M %p")))
    conn.commit()
    conn.close()

    return jsonify({"success": True, "message": f"{full_name} ({student_id}) enrolled."})

@app.route("/api/students/<original_id>", methods=["PUT"])
def update_student(original_id):
    data = request.json
    new_id = data.get("student_id", "").strip()
    full_name = " ".join(data.get("full_name", "").split())
    section = data.get("section", "CPE-3B").strip() or "CPE-3B"
    image_data = data.get("image", "")

    if not new_id or not full_name:
        return jsonify({"success": False, "message": "Student ID and Full Name are required."}), 400

    conn = get_db()
    current_row = conn.execute("SELECT * FROM students WHERE student_id = ?", (original_id,)).fetchone()
    if not current_row:
        conn.close()
        return jsonify({"success": False, "message": "Student record not found."}), 404

    if conn.execute("SELECT 1 FROM students WHERE LOWER(student_id) = LOWER(?) AND student_id != ?", (new_id, original_id)).fetchone():
        conn.close()
        return jsonify({"success": False, "message": f"Student ID '{new_id}' is already in use."}), 409

    if conn.execute("SELECT 1 FROM students WHERE LOWER(full_name) = LOWER(?) AND student_id != ?", (full_name, original_id)).fetchone():
        conn.close()
        return jsonify({"success": False, "message": f"'{full_name}' is already registered under another ID."}), 409

    emb_filename = current_row["embedding_file"]
    old_emb_path = os.path.join("enrolled_faces", emb_filename)

    if image_data:
        feature, err = extract_face_feature(image_data, exclude_student_id=original_id)
        if err:
            conn.close()
            return jsonify({"success": False, "message": err}), 400

        safe_id = "".join(c for c in new_id if c.isalnum() or c in ("-", "_"))
        emb_filename = f"{safe_id}.npy"
        new_emb_path = os.path.join("enrolled_faces", emb_filename)
        if old_emb_path != new_emb_path and os.path.exists(old_emb_path):
            os.remove(old_emb_path)
        np.save(new_emb_path, feature)
        save_reference_portrait(image_data, new_id, full_name)
    elif new_id != original_id:
        safe_id = "".join(c for c in new_id if c.isalnum() or c in ("-", "_"))
        emb_filename = f"{safe_id}.npy"
        new_emb_path = os.path.join("enrolled_faces", emb_filename)
        if os.path.exists(old_emb_path) and old_emb_path != new_emb_path:
            os.rename(old_emb_path, new_emb_path)

    conn.execute("""
        UPDATE students
        SET student_id = ?, full_name = ?, section = ?, embedding_file = ?
        WHERE student_id = ?
    """, (new_id, full_name, section, emb_filename, original_id))

    conn.execute("UPDATE attendance_logs SET student_id = ?, full_name = ? WHERE student_id = ?", (new_id, full_name, original_id))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": f"Updated {full_name} ({new_id})."})

@app.route("/api/students/<student_id>", methods=["DELETE"])
def delete_student(student_id):
    conn = get_db()
    row = conn.execute("SELECT embedding_file FROM students WHERE student_id = ?", (student_id,)).fetchone()
    if row:
        emb_path = os.path.join("enrolled_faces", row["embedding_file"])
        if os.path.exists(emb_path):
            os.remove(emb_path)
        safe_id = "".join(c for c in student_id if c.isalnum() or c in ("-", "_"))
        for fname in os.listdir("enrolled_faces"):
            if fname.startswith(f"{safe_id}_") and fname.endswith(".jpg"):
                os.remove(os.path.join("enrolled_faces", fname))
        conn.execute("DELETE FROM students WHERE student_id = ?", (student_id,))
        conn.commit()
    conn.close()
    return jsonify({"success": True})

@app.route("/api/schedules", methods=["POST"])
def add_schedule():
    data = request.json
    day_of_week = data.get("day_of_week", "").strip()
    subject_code = " ".join(data.get("subject_code", "").split()).upper()
    subject = " ".join(data.get("subject", "").split())
    start_time = data.get("start_time", "").strip()
    end_time = data.get("end_time", "").strip()

    conn = get_db()
    err = validate_schedule_input(conn, day_of_week, subject_code, subject, start_time, end_time)
    if err:
        conn.close()
        return jsonify({"success": False, "message": err}), 400

    conn.execute(
        "INSERT INTO schedules (day_of_week, subject_code, subject, start_time, end_time) VALUES (?, ?, ?, ?, ?)",
        (day_of_week, subject_code, subject, start_time, end_time),
    )
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": f"Added {subject_code} ({day_of_week})."})

@app.route("/api/schedules/<int:sched_id>", methods=["PUT"])
def update_schedule(sched_id):
    data = request.json
    day_of_week = data.get("day_of_week", "").strip()
    subject_code = " ".join(data.get("subject_code", "").split()).upper()
    subject = " ".join(data.get("subject", "").split())
    start_time = data.get("start_time", "").strip()
    end_time = data.get("end_time", "").strip()

    conn = get_db()
    existing = conn.execute("SELECT * FROM schedules WHERE id = ?", (sched_id,)).fetchone()
    if not existing:
        conn.close()
        return jsonify({"success": False, "message": "Schedule not found."}), 404

    err = validate_schedule_input(conn, day_of_week, subject_code, subject, start_time, end_time, exclude_id=sched_id)
    if err:
        conn.close()
        return jsonify({"success": False, "message": err}), 400

    conn.execute("""
        UPDATE schedules
        SET day_of_week = ?, subject_code = ?, subject = ?, start_time = ?, end_time = ?
        WHERE id = ?
    """, (day_of_week, subject_code, subject, start_time, end_time, sched_id))

    conn.execute("UPDATE attendance_logs SET subject_code = ?, subject = ? WHERE subject = ?", (subject_code, subject, existing["subject"]))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": f"Updated {subject_code} ({day_of_week})."})

@app.route("/api/schedules/<int:sched_id>", methods=["DELETE"])
def delete_schedule(sched_id):
    conn = get_db()
    conn.execute("DELETE FROM schedules WHERE id = ?", (sched_id,))
    conn.commit()
    conn.close()
    return jsonify({"success": True})

@app.route("/api/verify_frame", methods=["POST"])
def verify_frame():
    override_subject = None
    if request.is_json:
        data = request.json
        raw_image = data.get("image", "")
        override_subject = data.get("override_subject") or None
    else:
        raw_image = request.data
        esp32_node["last_seen"] = time.time()

    pending_response, verified_student = verify_and_check_liveness(raw_image)
    if pending_response:
        status_code = 400 if pending_response.get("status") == "ERROR" else 200
        return jsonify(pending_response), status_code

    subject_code, subject, att_status = get_active_schedule(override_subject)
    if not subject:
        today_name = datetime.now().strftime("%A")
        return jsonify({
            "status": "NO_CLASS",
            "student_name": verified_student["full_name"],
            "message": f"No Active Class ({today_name})"
        })

    now_dt = datetime.now()
    date_str = now_dt.strftime("%Y-%m-%d")
    time_str = now_dt.strftime("%I:%M:%S %p")
    display_subj = subject_code or subject

    conn = get_db()
    try:
        conn.execute("""
            INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (verified_student["student_id"], verified_student["full_name"], subject_code or "", subject, att_status, date_str, time_str))
        conn.commit()
        msg = f"{verified_student['full_name']} • {att_status} ({display_subj})"
        log_state = "LOGGED"
    except sqlite3.IntegrityError:
        msg = f"{verified_student['full_name']} • Already Logged ({display_subj})"
        log_state = "ALREADY_LOGGED"
    finally:
        conn.close()

    return jsonify({
        "status": log_state,
        "student_name": verified_student["full_name"],
        "subject_code": subject_code,
        "subject": subject,
        "attendance_status": att_status,
        "message": msg
    })

@app.route("/api/mark_absences", methods=["POST"])
def mark_absences():
    subject = request.json.get("subject", "").strip()
    if not subject:
        return jsonify({"success": False, "message": "Select a subject first."}), 400

    now_dt = datetime.now()
    date_str = now_dt.strftime("%Y-%m-%d")
    time_str = now_dt.strftime("%I:%M:%S %p")

    conn = get_db()
    sched = conn.execute("SELECT subject_code FROM schedules WHERE subject = ?", (subject,)).fetchone()
    subject_code = sched["subject_code"] if sched else ""

    students = conn.execute("SELECT * FROM students").fetchall()
    added = 0
    for s in students:
        try:
            conn.execute("""
                INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str)
                VALUES (?, ?, ?, ?, 'ABSENT', ?, ?)
            """, (s["student_id"], s["full_name"], subject_code, subject, date_str, time_str))
            added += 1
        except sqlite3.IntegrityError:
            pass
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": f"Marked {added} student(s) ABSENT for {subject}."})

@app.route("/api/logs/clear", methods=["DELETE"])
def clear_logs():
    conn = get_db()
    conn.execute("DELETE FROM attendance_logs")
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": "All attendance logs cleared."})

@app.route("/api/export_csv", methods=["GET"])
def export_csv():
    subject_filter = request.args.get("subject", "").strip()
    date_filter = request.args.get("date", "").strip()
    student_filter = request.args.get("student_id", "").strip()

    query = "SELECT student_id, full_name, subject_code, subject, status, date_str, time_str FROM attendance_logs WHERE 1=1"
    params = []
    if subject_filter:
        query += " AND subject = ?"
        params.append(subject_filter)
    if date_filter:
        query += " AND date_str = ?"
        params.append(date_filter)
    if student_filter:
        query += " AND student_id = ?"
        params.append(student_filter)
    query += " ORDER BY date_str DESC, id DESC"

    conn = get_db()
    logs = conn.execute(query, params).fetchall()
    conn.close()

    output = io.StringIO()
    output.write("\ufeff")
    writer = csv.writer(output, quoting=csv.QUOTE_MINIMAL)
    writer.writerow(["Student ID", "Full Name", "Subject Code", "Subject Name", "Status", "Date", "Time"])
    for r in logs:
        writer.writerow([
            f" {r['student_id']} ",
            f" {r['full_name']} ",
            f" {r['subject_code']} ",
            f" {r['subject']} ",
            f" {r['status']} ",
            f" {r['date_str']} ",
            f" {r['time_str']} ",
        ])

    timestamp_12h = datetime.now().strftime("%m-%d-%Y_%I-%M-%S%p")
    filename = f"attendance_log_{timestamp_12h}.csv"

    return Response(
        output.getvalue(),
        mimetype="text/csv; charset=utf-8",
        headers={"Content-Disposition": f"attachment;filename={filename}"},
    )

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)