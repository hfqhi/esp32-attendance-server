import csv
import os
import sqlite3
from datetime import datetime

DB_PATH = "attendance.db"

VALID_DAYS = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def sanitize_db_slug(text, fallback="General"):
    clean = "_".join(str(text).strip().split())
    clean = "".join(c for c in clean if c.isalnum() or c in ("-", "_"))
    return clean or fallback


def init_db():
    conn = get_db()
    cursor = conn.cursor()

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS students (
            student_id TEXT PRIMARY KEY,
            full_name TEXT NOT NULL,
            section TEXT DEFAULT 'CPE-3B',
            embedding_file TEXT NOT NULL,
            created_at TEXT NOT NULL
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS schedules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            day_of_week TEXT NOT NULL DEFAULT 'Monday',
            subject_code TEXT NOT NULL DEFAULT '',
            subject TEXT NOT NULL,
            start_time TEXT NOT NULL,
            end_time TEXT NOT NULL
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS attendance_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            student_id TEXT NOT NULL,
            full_name TEXT NOT NULL,
            subject_code TEXT NOT NULL DEFAULT '',
            subject TEXT NOT NULL,
            status TEXT NOT NULL,
            date_str TEXT NOT NULL,
            time_str TEXT NOT NULL,
            proof_image TEXT NOT NULL DEFAULT '',
            UNIQUE(student_id, subject, date_str)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS sd_sync_queue (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            action TEXT NOT NULL,
            sd_path TEXT NOT NULL,
            local_rel_path TEXT NOT NULL DEFAULT '',
            payload TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL
        )
    """)

    sched_cols = [r["name"] for r in cursor.execute("PRAGMA table_info(schedules)").fetchall()]
    if "day_of_week" not in sched_cols:
        cursor.execute("ALTER TABLE schedules ADD COLUMN day_of_week TEXT NOT NULL DEFAULT 'Monday'")
    if "subject_code" not in sched_cols:
        cursor.execute("ALTER TABLE schedules ADD COLUMN subject_code TEXT NOT NULL DEFAULT ''")

    log_cols = [r["name"] for r in cursor.execute("PRAGMA table_info(attendance_logs)").fetchall()]
    if "subject_code" not in log_cols:
        cursor.execute("ALTER TABLE attendance_logs ADD COLUMN subject_code TEXT NOT NULL DEFAULT ''")
    if "proof_image" not in log_cols:
        cursor.execute("ALTER TABLE attendance_logs ADD COLUMN proof_image TEXT NOT NULL DEFAULT ''")

    conn.commit()
    conn.close()


def queue_sd_sync(conn, action, sd_path, local_rel_path="", payload=""):
    """Queues an incremental sync instruction for the ESP32-CAM MicroSD database."""
    conn.execute("DELETE FROM sd_sync_queue WHERE sd_path = ? AND action = ?", (sd_path, action))
    if action == "DELETE_FILE":
        conn.execute("DELETE FROM sd_sync_queue WHERE sd_path = ? AND action = 'DOWNLOAD_FILE'", (sd_path,))
    elif action == "DOWNLOAD_FILE":
        conn.execute("DELETE FROM sd_sync_queue WHERE sd_path = ? AND action = 'DELETE_FILE'", (sd_path,))

    conn.execute("""
        INSERT INTO sd_sync_queue (action, sd_path, local_rel_path, payload, created_at)
        VALUES (?, ?, ?, ?, ?)
    """, (action, sd_path, local_rel_path, payload, datetime.now().strftime("%Y-%m-%d %H:%M:%S")))


def sync_daily_csv_both_ends(conn, date_str, subject_code="", subject_name=""):
    """
    Generates identical daily CSV logs at:
      Local: attendance_logs/<Date>/<SubjectCode>/<Date>_<SubjectCode>_attendance.csv
      SD:    /attendance_logs/<Date>/<SubjectCode>/<Date>_<SubjectCode>_attendance.csv
    """
    try:
        dt_obj = datetime.strptime(date_str, "%Y-%m-%d")
        day_name = dt_obj.strftime("%A")
    except ValueError:
        day_name = datetime.now().strftime("%A")

    subj_slug = sanitize_db_slug(subject_code or subject_name or "General")
    date_slug = sanitize_db_slug(date_str, "0000-00-00")

    query = "SELECT student_id, full_name, subject_code, subject, status, date_str, time_str, proof_image FROM attendance_logs WHERE date_str = ?"
    params = [date_str]
    if subject_code:
        query += " AND subject_code = ?"
        params.append(subject_code)
    elif subject_name:
        query += " AND subject = ?"
        params.append(subject_name)
    query += " ORDER BY id ASC"

    rows = conn.execute(query, params).fetchall()

    csv_filename = f"{date_slug}_{subj_slug}_attendance.csv"
    local_dir = os.path.join("attendance_logs", date_slug, subj_slug)
    os.makedirs(local_dir, exist_ok=True)
    local_csv_path = os.path.join(local_dir, csv_filename)
    sd_csv_path = f"/attendance_logs/{date_slug}/{subj_slug}/{csv_filename}"

    if not rows:
        if os.path.exists(local_csv_path):
            os.remove(local_csv_path)
        queue_sd_sync(conn, "DELETE_FILE", sd_csv_path)
        return ""

    with open(local_csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["Student ID", "Student Name", "Subject Code", "Subject Name", "Status", "Day", "Date", "Time", "Capture File"])
        for r in rows:
            capture_fname = os.path.basename(r["proof_image"]) if r["proof_image"] else ""
            writer.writerow([
                r["student_id"],
                r["full_name"],
                r["subject_code"] or subj_slug,
                r["subject"],
                r["status"],
                day_name,
                r["date_str"],
                r["time_str"],
                capture_fname,
            ])

    local_rel = os.path.join("attendance_logs", date_slug, subj_slug, csv_filename).replace("\\", "/")
    queue_sd_sync(conn, "DOWNLOAD_FILE", sd_csv_path, local_rel_path=local_rel)
    return local_rel


def rebuild_all_daily_csvs(conn):
    """Ensures every (date_str, subject_code, subject) in attendance_logs has an up-to-date local CSV."""
    groups = conn.execute("SELECT DISTINCT date_str, subject_code, subject FROM attendance_logs").fetchall()
    generated = []
    for g in groups:
        rel = sync_daily_csv_both_ends(conn, g["date_str"], subject_code=g["subject_code"], subject_name=g["subject"])
        if rel:
            generated.append(rel)
    return generated


def time_to_minutes(t_str):
    h, m = map(int, t_str.split(":"))
    return h * 60 + m


def validate_schedule_input(conn, day_of_week, subject_code, subject, start_time, end_time, exclude_id=None):
    if day_of_week not in VALID_DAYS:
        return "Please select a valid day of the week."
    if not subject_code or not subject or not start_time or not end_time:
        return "Day, Subject Code, Subject Name, Start Time, and End Time are required."

    new_start = time_to_minutes(start_time)
    new_end = time_to_minutes(end_time)
    if new_end <= new_start:
        return "End Time must be later than Start Time."

    query = "SELECT id, subject_code, subject, start_time, end_time FROM schedules WHERE day_of_week = ?"
    params = [day_of_week]
    if exclude_id is not None:
        query += " AND id != ?"
        params.append(exclude_id)

    for row in conn.execute(query, params).fetchall():
        if row["subject_code"].strip().lower() == subject_code.lower() and row["start_time"] == start_time:
            return f"Duplicate schedule: {subject_code} on {day_of_week} at {start_time} already exists."

        ex_start = time_to_minutes(row["start_time"])
        ex_end = time_to_minutes(row["end_time"])
        if new_start < ex_end and new_end > ex_start:
            label = f"{row['subject_code']} ({row['subject']})" if row["subject_code"] else row["subject"]
            return f"Time overlap on {day_of_week} with {label} ({row['start_time']} - {row['end_time']})."

    return None


def get_active_schedule():
    """Returns (subject_code, subject_name, status) if current day & time falls within a scheduled class."""
    now = datetime.now()
    current_day = now.strftime("%A")
    now_mins = now.hour * 60 + now.minute

    conn = get_db()
    schedules = conn.execute("SELECT * FROM schedules WHERE day_of_week = ?", (current_day,)).fetchall()
    conn.close()

    for sched in schedules:
        start_mins = time_to_minutes(sched["start_time"])
        end_mins = time_to_minutes(sched["end_time"])
        if start_mins <= now_mins <= end_mins:
            status = "LATE" if (now_mins - start_mins) > 15 else "PRESENT"
            return sched["subject_code"], sched["subject"], status

    return None, None, None


def auto_mark_ended_classes():
    """Marks unregistered enrolled students as ABSENT once today's class end_time passes, updating daily CSVs."""
    now = datetime.now()
    current_day = now.strftime("%A")
    today_str = now.strftime("%Y-%m-%d")
    now_mins = now.hour * 60 + now.minute

    conn = get_db()
    students = conn.execute("SELECT student_id, full_name FROM students").fetchall()
    if not students:
        conn.close()
        return

    schedules = conn.execute("SELECT * FROM schedules WHERE day_of_week = ?", (current_day,)).fetchall()
    changed_subjects = set()

    for sched in schedules:
        end_mins = time_to_minutes(sched["end_time"])
        if now_mins > end_mins:
            for stu in students:
                exists = conn.execute(
                    "SELECT 1 FROM attendance_logs WHERE student_id = ? AND subject = ? AND date_str = ?",
                    (stu["student_id"], sched["subject"], today_str),
                ).fetchone()
                if not exists:
                    conn.execute("""
                        INSERT INTO attendance_logs (student_id, full_name, subject_code, subject, status, date_str, time_str, proof_image)
                        VALUES (?, ?, ?, ?, 'ABSENT', ?, ?, '')
                    """, (stu["student_id"], stu["full_name"], sched["subject_code"], sched["subject"], today_str, sched["end_time"]))
                    changed_subjects.add((sched["subject_code"], sched["subject"]))

    for subj_code, subj_name in changed_subjects:
        sync_daily_csv_both_ends(conn, today_str, subject_code=subj_code, subject_name=subj_name)

    conn.commit()
    conn.close()