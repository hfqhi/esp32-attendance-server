import csv
import io
import os
import sqlite3
import time
import urllib.request
from datetime import datetime

import numpy as np
from flask import Flask, Response, jsonify, render_template, request, send_file, send_from_directory

from database import (
    auto_mark_ended_classes,
    get_active_schedule,
    get_db,
    init_db,
    queue_sd_sync,
    rebuild_all_daily_csvs,
    sync_daily_csv_both_ends,
    validate_schedule_input,
)
from face_engine import (
    extract_face_feature,
    identify_offline_face,
    sanitize_slug,
    save_proof_snapshot,
    save_reference_portrait,
    verify_instant_presence,
)

app = Flask(__name__)
init_db()

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


def resolve_student_photo_url(student_id):
    safe_id = sanitize_slug(student_id)
    if os.path.isdir("enrolled_faces"):
        for fname in sorted(os.listdir("enrolled_faces")):
            if fname.startswith(f"{safe_id}_") and fname.lower().endswith(".jpg"):
                return f"/api/esp32/portraits/{fname}"
    return ""


def proof_rel_to_sd_path(proof_rel):
    """Maps local captured_proofs/<Date>/<Subj>/<File>.jpg -> /captured_proofs/<Date>/<Subj>/<File>.jpg"""
    if not proof_rel:
        return ""
    clean_rel = proof_rel.replace("\\", "/").lstrip("/")
    return f"/captured_proofs/{clean_rel}"


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/proofs/<path:filename>", methods=["GET"])
def serve_proof_image(filename):
    return send_from_directory("captured_proofs", filename)


# --- ESP32-CAM DEVICE & FULL 1:1 MICROSD SYNC ENDPOINTS ---
@app.route("/api/esp32/register", methods=["POST"])
def register_esp32():
    data = request.json or {}
    ip_addr = data.get("ip") or request.remote_addr
    esp32_node["ip"] = ip_addr
    esp32_node["last_seen"] = time.time()

    conn = get_db()
    pending_count = conn.execute("SELECT COUNT(*) FROM sd_sync_queue").fetchone()[0]
    conn.close()

    return jsonify({
        "success": True,
        "ip": ip_addr,
        "pending_sync": pending_count,
        "server_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    })


@app.route("/api/esp32/proxy_capture", methods=["GET"])
def proxy_esp32_capture():
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
    files = [
        f for f in os.listdir("enrolled_faces")
        if f.lower().endswith(".jpg")
    ]
    return jsonify({"portraits": sorted(files)})


@app.route("/api/esp32/portraits/<path:filename>", methods=["GET"])
def download_portrait(filename):
    return send_from_directory("enrolled_faces", filename)


@app.route("/api/esp32/reconcile_manifest", methods=["GET"])
def get_reconcile_manifest():
    """
    Returns the complete, authoritative 1:1 list of all files that must exist on the SD card:
      - /enrolled_faces/<StudentID>_<StudentName>.jpg
      - /captured_proofs/<Date>/<SubjectCode>/<Date>_<Time>_<StudentID>_<StudentName>_<Status>.jpg
      - /attendance_logs/<Date>/<SubjectCode>/<Date>_<SubjectCode>_attendance.csv
    """
    esp32_node["last_seen"] = time.time()
    conn = get_db()
    csv_rels = rebuild_all_daily_csvs(conn)
    log_rows = conn.execute("SELECT proof_image FROM attendance_logs WHERE proof_image != ''").fetchall()
    conn.commit()
    conn.close()

    files = []

    if os.path.isdir("enrolled_faces"):
        for fname in sorted(os.listdir("enrolled_faces")):
            if fname.lower().endswith(".jpg"):
                files.append({
                    "sd_path": f"/enrolled_faces/{fname}",
                    "local_rel_path": f"enrolled_faces/{fname}",
                    "overwrite": False,
                })

    for r in log_rows:
        proof_rel = r["proof_image"].replace("\\", "/").lstrip("/")
        abs_local = os.path.join("captured_proofs", proof_rel)
        if os.path.exists(abs_local):
            files.append({
                "sd_path": f"/captured_proofs/{proof_rel}",
                "local_rel_path": f"captured_proofs/{proof_rel}",
                "overwrite": False,
            })

    for csv_rel in sorted(set(csv_rels)):
        clean_csv = csv_rel.replace("\\", "/").lstrip("/")
        if os.path.exists(clean_csv):
            files.append({
                "sd_path": f"/{clean_csv}",
                "local_rel_path": clean_csv,
                "overwrite": True,
            })

    return jsonify({"files": files})


@app.route("/api/esp32/sync_tasks", methods=["GET"])
def get_esp32_sync_tasks():
    """Returns pending local->SD file deletions, renames, captures, and CSV updates."""
    esp32_node["last_seen"] = time.time()
    conn = get_db()
    rows = conn.execute("""
        SELECT id, action, sd_path, local_rel_path
        FROM sd_sync_queue
        ORDER BY id ASC
        LIMIT 15
    """).fetchall()
    conn.close()

    tasks = []
    for r in rows:
        if r["action"] == "DOWNLOAD_FILE":
            abs_local = os.path.abspath(r["local_rel_path"])
            if not os.path.exists(abs_local):
                c2 = get_db()
                c2.execute("DELETE FROM sd_sync_queue WHERE id = ?", (r["id"],))
                c2.commit()
                c2.close()
                continue
        tasks.append({
            "id": r["id"],
            "action": r["action"],
            "sd_path": r["sd_path"],
            "local_rel_path": r["local_rel_path"],
        })

    return jsonify({"tasks": tasks})


@app.route("/api/esp32/download_sync_file", methods=["GET"])
def download_sync_file():
    """Streams any queued portrait, capture photo, or daily CSV to the ESP32-CAM."""
    esp32_node["last_seen"] = time.time()
    rel_path = request.args.get("path", "").strip().lstrip("/\\")
    if not rel_path:
        return jsonify({"error": "Missing path"}), 400

    allowed_roots = (
        os.path.abspath("enrolled_faces"),
        os.path.abspath("captured_proofs"),
        os.path.abspath("attendance_logs"),
    )
    abs_target = os.path.abspath(rel_path)
    if not any(abs_target.startswith(root + os.sep) for root in allowed_roots):
        return jsonify({"error": "Access denied"}), 403

    if not os.path.exists(abs_target):
        return jsonify({"error": "File not found"}), 404

    return send_file(abs_target)


@app.route("/api/esp32/sync_ack", methods=["POST"])
def ack_esp32_sync_task():
    esp32_node["last_seen"] = time.time()
    data = request.json or {}
    task_id = data.get("id")
    if task_id is not None:
        conn = get_db()
        conn.execute("DELETE FROM sd_sync_queue WHERE id = ?", (task_id,))
        conn.commit()
        conn.close()
    return jsonify({"success": True})


@app.route("/api/esp32/sync_offline", methods=["POST"])
def sync_offline_capture():
    """Processes buffered offline captures from ESP32-CAM and updates both local DB & SD card."""
    esp32_node["last_seen"] = time.time()
    raw_bytes = request.data

    student, raw_frame = identify_offline_face(raw_bytes)
    if not student:
        return jsonify({"status": "IGNORED", "student_name": "", "message": "No centered enrolled face matched"})

    subject_code, subject, att_status = get_active_schedule()
    if not subject:
        subject_code = "OFFLINE"
        subject = "Offline Sync"
        att_status = "PRESENT"

    now_dt = datetime.now()
    date_str = now_dt.strftime("%Y-%m-%d")
    time_str = now_dt.strftime("%I:%M:%S %p")

    conn = get_db()
    existing = conn.execute(
        "SELECT id FROM attendance_logs WHERE student_id = ? AND subject = ? AND date_str = ?",
        (student["student_id"], subject, date_str),
    ).fetchone()

    if existing:
        conn.close()
        return jsonify({
            "status": "ALREADY_LOGGED",
            "student_id": student["student_id"],
            "student_name": student["full_name"],
            "subject_code": subject_code,
            "attendance_status": att_status,
            "message": "Already logged today",
        })

    proof_rel, sd_target = save_proof_snapshot(
        raw_frame,
        student["student_id"],
        student["full_name"],
        subject_code,
        status=att_status,
    )

    try:
        conn.execute("""
            INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str, proof_image)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (student["student_id"], student["full_name"], subject_code or "", subject, att_status, date_str, time_str, proof_rel))

        if proof_rel and sd_target:
            queue_sd_sync(conn, "DOWNLOAD_FILE", sd_target, local_rel_path=f"captured_proofs/{proof_rel}")
        sync_daily_csv_both_ends(conn, date_str, subject_code=subject_code, subject_name=subject)
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
    auto_mark_ended_classes()

    conn = get_db()
    students = []
    for r in conn.execute("SELECT * FROM students ORDER BY full_name").fetchall():
        stu_dict = dict(r)
        stu_dict["photo_url"] = resolve_student_photo_url(stu_dict["student_id"])
        students.append(stu_dict)

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
    active_label = f"{active_code} • {active_subject}" if active_subject else "No Active Class"

    return jsonify({
        "students": students,
        "schedules": schedules,
        "logs": logs,
        "active_subject": active_label,
        "esp32": get_esp32_status(),
    })


@app.route("/api/enroll", methods=["POST"])
def enroll_student():
    data = request.json
    student_id = data.get("student_id", "").strip()
    full_name = " ".join(data.get("full_name", "").split())
    section = data.get("section", "CPE-3B").strip() or "CPE-3B"
    image_data = data.get("image", "")
    camera_source = data.get("source", "webcam")

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

    feature, err = extract_face_feature(image_data, source=camera_source)
    if err:
        return jsonify({"success": False, "message": err}), 400

    safe_id = "".join(c for c in student_id if c.isalnum() or c in ("-", "_"))
    emb_filename = f"{safe_id}.npy"
    np.save(os.path.join("enrolled_faces", emb_filename), feature)
    portrait_fname = save_reference_portrait(image_data, student_id, full_name)

    conn = get_db()
    conn.execute("""
        INSERT INTO students (student_id, full_name, section, embedding_file, created_at)
        VALUES (?, ?, ?, ?, ?)
    """, (student_id, full_name, section, emb_filename, datetime.now().strftime("%Y-%m-%d %I:%M %p")))

    if portrait_fname:
        queue_sd_sync(
            conn,
            "DOWNLOAD_FILE",
            f"/enrolled_faces/{portrait_fname}",
            local_rel_path=f"enrolled_faces/{portrait_fname}",
        )

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
    camera_source = data.get("source", "webcam")

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
    old_safe_id = sanitize_slug(original_id)

    if image_data:
        feature, err = extract_face_feature(image_data, exclude_student_id=original_id, source=camera_source)
        if err:
            conn.close()
            return jsonify({"success": False, "message": err}), 400

        safe_id = "".join(c for c in new_id if c.isalnum() or c in ("-", "_"))
        emb_filename = f"{safe_id}.npy"
        new_emb_path = os.path.join("enrolled_faces", emb_filename)
        if old_emb_path != new_emb_path and os.path.exists(old_emb_path):
            os.remove(old_emb_path)
        np.save(new_emb_path, feature)

        for fname in os.listdir("enrolled_faces"):
            if fname.startswith(f"{old_safe_id}_") and fname.endswith(".jpg"):
                os.remove(os.path.join("enrolled_faces", fname))
                queue_sd_sync(conn, "DELETE_FILE", f"/enrolled_faces/{fname}")

        portrait_fname = save_reference_portrait(image_data, new_id, full_name)
        if portrait_fname:
            queue_sd_sync(
                conn,
                "DOWNLOAD_FILE",
                f"/enrolled_faces/{portrait_fname}",
                local_rel_path=f"enrolled_faces/{portrait_fname}",
            )
    elif new_id != original_id or full_name != current_row["full_name"]:
        safe_id = "".join(c for c in new_id if c.isalnum() or c in ("-", "_"))
        emb_filename = f"{safe_id}.npy"
        new_emb_path = os.path.join("enrolled_faces", emb_filename)
        if os.path.exists(old_emb_path) and old_emb_path != new_emb_path:
            os.rename(old_emb_path, new_emb_path)

        new_safe_id = sanitize_slug(new_id)
        new_safe_name = sanitize_slug(full_name)
        new_portrait_fname = f"{new_safe_id}_{new_safe_name}.jpg"

        for fname in os.listdir("enrolled_faces"):
            if fname.startswith(f"{old_safe_id}_") and fname.endswith(".jpg"):
                os.rename(
                    os.path.join("enrolled_faces", fname),
                    os.path.join("enrolled_faces", new_portrait_fname),
                )
                if fname != new_portrait_fname:
                    queue_sd_sync(conn, "DELETE_FILE", f"/enrolled_faces/{fname}")
                queue_sd_sync(
                    conn,
                    "DOWNLOAD_FILE",
                    f"/enrolled_faces/{new_portrait_fname}",
                    local_rel_path=f"enrolled_faces/{new_portrait_fname}",
                )

    conn.execute("""
        UPDATE students
        SET student_id = ?, full_name = ?, section = ?, embedding_file = ?
        WHERE student_id = ?
    """, (new_id, full_name, section, emb_filename, original_id))

    affected_logs = conn.execute(
        "SELECT id, subject_code, subject, date_str, proof_image FROM attendance_logs WHERE student_id = ?",
        (original_id,),
    ).fetchall()

    affected_csvs = set()
    for log_row in affected_logs:
        affected_csvs.add((log_row["date_str"], log_row["subject_code"], log_row["subject"]))
        old_proof = log_row["proof_image"]
        if old_proof:
            parts = old_proof.replace("\\", "/").split("/")
            if len(parts) >= 3:
                date_slug, subj_slug, old_fname = parts[0], parts[1], parts[2]
                f_parts = old_fname.rsplit(".", 1)[0].split("_")
                if len(f_parts) >= 5:
                    date_part, time_part, status_part = f_parts[0], f_parts[1], f_parts[-1]
                    new_fname = f"{date_part}_{time_part}_{sanitize_slug(new_id)}_{sanitize_slug(full_name)}_{status_part}.jpg"
                    if new_fname != old_fname:
                        old_abs = os.path.join("captured_proofs", date_slug, subj_slug, old_fname)
                        new_abs = os.path.join("captured_proofs", date_slug, subj_slug, new_fname)
                        if os.path.exists(old_abs):
                            os.rename(old_abs, new_abs)
                        new_proof_rel = f"{date_slug}/{subj_slug}/{new_fname}"
                        conn.execute("UPDATE attendance_logs SET proof_image = ? WHERE id = ?", (new_proof_rel, log_row["id"]))

                        queue_sd_sync(conn, "DELETE_FILE", f"/captured_proofs/{date_slug}/{subj_slug}/{old_fname}")
                        queue_sd_sync(
                            conn,
                            "DOWNLOAD_FILE",
                            f"/captured_proofs/{date_slug}/{subj_slug}/{new_fname}",
                            local_rel_path=f"captured_proofs/{new_proof_rel}",
                        )

    conn.execute("UPDATE attendance_logs SET student_id = ?, full_name = ? WHERE student_id = ?", (new_id, full_name, original_id))

    for d_str, s_code, s_name in affected_csvs:
        sync_daily_csv_both_ends(conn, d_str, subject_code=s_code, subject_name=s_name)

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
        safe_id = sanitize_slug(student_id)
        for fname in os.listdir("enrolled_faces"):
            if fname.startswith(f"{safe_id}_") and fname.endswith(".jpg"):
                os.remove(os.path.join("enrolled_faces", fname))
                queue_sd_sync(conn, "DELETE_FILE", f"/enrolled_faces/{fname}")
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

    affected_dates = [
        r["date_str"]
        for r in conn.execute("SELECT DISTINCT date_str FROM attendance_logs WHERE subject = ?", (existing["subject"],)).fetchall()
    ]
    conn.execute("UPDATE attendance_logs SET subject_code = ?, subject = ? WHERE subject = ?", (subject_code, subject, existing["subject"]))
    for d_str in affected_dates:
        sync_daily_csv_both_ends(conn, d_str, subject_code=subject_code, subject_name=subject)

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
    auto_mark_ended_classes()

    if request.is_json:
        data = request.json
        raw_image = data.get("image", "")
        camera_source = data.get("source", "webcam")
    else:
        raw_image = request.data
        camera_source = request.headers.get("X-Camera-Source", "esp32")
        esp32_node["last_seen"] = time.time()

    pending_response, verified_student, raw_frame = verify_instant_presence(raw_image, source=camera_source)
    if pending_response:
        status_code = 400 if pending_response.get("status") == "ERROR" else 200
        return jsonify(pending_response), status_code

    subject_code, subject, att_status = get_active_schedule()
    if not subject:
        return jsonify({
            "status": "NO_CLASS",
            "student_id": verified_student["student_id"],
            "student_name": verified_student["full_name"],
            "message": "No Active Class"
        })

    now_dt = datetime.now()
    date_str = now_dt.strftime("%Y-%m-%d")
    time_str = now_dt.strftime("%I:%M:%S %p")
    display_subj = subject_code or subject

    conn = get_db()
    existing = conn.execute(
        "SELECT id FROM attendance_logs WHERE student_id = ? AND subject = ? AND date_str = ?",
        (verified_student["student_id"], subject, date_str),
    ).fetchone()

    if existing:
        conn.close()
        return jsonify({
            "status": "ALREADY_LOGGED",
            "student_id": verified_student["student_id"],
            "student_name": verified_student["full_name"],
            "subject_code": subject_code,
            "subject": subject,
            "attendance_status": att_status,
            "message": f"{verified_student['full_name']} • Already Logged ({display_subj})"
        })

    proof_rel, sd_target = save_proof_snapshot(
        raw_frame,
        verified_student["student_id"],
        verified_student["full_name"],
        subject_code,
        status=att_status,
    )
    conn.execute("""
        INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str, proof_image)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """, (verified_student["student_id"], verified_student["full_name"], subject_code or "", subject, att_status, date_str, time_str, proof_rel))

    if proof_rel and sd_target:
        queue_sd_sync(conn, "DOWNLOAD_FILE", sd_target, local_rel_path=f"captured_proofs/{proof_rel}")
    sync_daily_csv_both_ends(conn, date_str, subject_code=subject_code, subject_name=subject)

    conn.commit()
    conn.close()

    return jsonify({
        "status": "LOGGED",
        "student_id": verified_student["student_id"],
        "student_name": verified_student["full_name"],
        "subject_code": subject_code,
        "subject": subject,
        "attendance_status": att_status,
        "message": f"{verified_student['full_name']} • {att_status} ({display_subj})"
    })


@app.route("/api/logs/<int:log_id>", methods=["DELETE"])
def delete_single_log(log_id):
    conn = get_db()
    row = conn.execute("SELECT date_str, subject_code, subject, proof_image FROM attendance_logs WHERE id = ?", (log_id,)).fetchone()
    if row:
        if row["proof_image"]:
            proof_path = os.path.join("captured_proofs", row["proof_image"])
            if os.path.exists(proof_path):
                os.remove(proof_path)
            sd_target = proof_rel_to_sd_path(row["proof_image"])
            if sd_target:
                queue_sd_sync(conn, "DELETE_FILE", sd_target)

        conn.execute("DELETE FROM attendance_logs WHERE id = ?", (log_id,))
        sync_daily_csv_both_ends(conn, row["date_str"], subject_code=row["subject_code"], subject_name=row["subject"])
        conn.commit()
    conn.close()
    return jsonify({"success": True})


@app.route("/api/logs/clear", methods=["DELETE"])
def clear_logs():
    conn = get_db()
    rows = conn.execute("SELECT date_str, subject_code, subject, proof_image FROM attendance_logs").fetchall()
    affected_csvs = set()

    for row in rows:
        affected_csvs.add((row["date_str"], row["subject_code"], row["subject"]))
        if row["proof_image"]:
            proof_path = os.path.join("captured_proofs", row["proof_image"])
            if os.path.exists(proof_path):
                os.remove(proof_path)
            sd_target = proof_rel_to_sd_path(row["proof_image"])
            if sd_target:
                queue_sd_sync(conn, "DELETE_FILE", sd_target)

    conn.execute("DELETE FROM attendance_logs")
    for d_str, s_code, s_name in affected_csvs:
        sync_daily_csv_both_ends(conn, d_str, subject_code=s_code, subject_name=s_name)

    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": "All attendance logs cleared and synced."})


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