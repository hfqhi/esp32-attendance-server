import sqlite3
from datetime import datetime, timedelta

DB_PATH = "attendance.db"


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    cur = conn.cursor()

    cur.execute("""
        CREATE TABLE IF NOT EXISTS students (
            student_id TEXT PRIMARY KEY,
            full_name TEXT NOT NULL,
            section TEXT DEFAULT 'CPE-3B',
            embedding_file TEXT NOT NULL,
            created_at TEXT NOT NULL
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS schedules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            day_of_week TEXT NOT NULL DEFAULT 'Monday',
            subject_code TEXT NOT NULL DEFAULT '',
            subject TEXT NOT NULL,
            start_time TEXT NOT NULL,
            end_time TEXT NOT NULL
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS attendance_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            student_id TEXT NOT NULL,
            full_name TEXT NOT NULL,
            subject_code TEXT DEFAULT '',
            subject TEXT NOT NULL,
            status TEXT NOT NULL,
            date_str TEXT NOT NULL,
            time_str TEXT NOT NULL,
            proof_image TEXT DEFAULT '',
            UNIQUE(student_id, subject, date_str)
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS spoof_attempts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            suspected_name TEXT NOT NULL,
            source TEXT NOT NULL,
            texture_score REAL NOT NULL,
            proof_image TEXT NOT NULL,
            date_str TEXT NOT NULL,
            time_str TEXT NOT NULL
        )
    """)

    # Safe migrations for existing databases
    sched_cols = [r["name"] for r in cur.execute("PRAGMA table_info(schedules)").fetchall()]
    if "day_of_week" not in sched_cols:
        cur.execute("ALTER TABLE schedules ADD COLUMN day_of_week TEXT NOT NULL DEFAULT 'Monday'")
    if "subject_code" not in sched_cols:
        cur.execute("ALTER TABLE schedules ADD COLUMN subject_code TEXT NOT NULL DEFAULT ''")

    log_cols = [r["name"] for r in cur.execute("PRAGMA table_info(attendance_logs)").fetchall()]
    if "subject_code" not in log_cols:
        cur.execute("ALTER TABLE attendance_logs ADD COLUMN subject_code TEXT DEFAULT ''")
    if "proof_image" not in log_cols:
        cur.execute("ALTER TABLE attendance_logs ADD COLUMN proof_image TEXT DEFAULT ''")

    conn.commit()
    conn.close()


def validate_schedule_input(conn, day_of_week, subject_code, subject, start_time, end_time, exclude_id=None):
    valid_days = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"}
    if day_of_week not in valid_days:
        return "Select a valid day of the week."
    if not subject_code or not subject or not start_time or not end_time:
        return "Day, subject code, subject name, start time, and end time are required."
    if start_time >= end_time:
        return "End time must be later than start time."

    if exclude_id is not None:
        dup = conn.execute("""
            SELECT 1 FROM schedules
            WHERE day_of_week = ? AND (LOWER(subject_code) = LOWER(?) OR LOWER(subject) = LOWER(?)) AND id != ?
        """, (day_of_week, subject_code, subject, exclude_id)).fetchone()
        existing = conn.execute(
            "SELECT * FROM schedules WHERE day_of_week = ? AND id != ?", (day_of_week, exclude_id)
        ).fetchall()
    else:
        dup = conn.execute("""
            SELECT 1 FROM schedules
            WHERE day_of_week = ? AND (LOWER(subject_code) = LOWER(?) OR LOWER(subject) = LOWER(?))
        """, (day_of_week, subject_code, subject)).fetchone()
        existing = conn.execute(
            "SELECT * FROM schedules WHERE day_of_week = ?", (day_of_week,)
        ).fetchall()

    if dup:
        return f"'{subject_code} — {subject}' already exists on {day_of_week}."

    for row in existing:
        if start_time < row["end_time"] and end_time > row["start_time"]:
            code_label = f"{row['subject_code']} ({row['subject']})" if row["subject_code"] else row["subject"]
            return f"Time conflict on {day_of_week} with {code_label} ({row['start_time']} - {row['end_time']})."

    return None


def get_active_schedule(override_subject=None):
    conn = get_db()
    today_day = datetime.now().strftime("%A")

    if override_subject:
        row = conn.execute(
            "SELECT * FROM schedules WHERE subject = ? AND day_of_week = ?",
            (override_subject, today_day),
        ).fetchone()
        if not row:
            row = conn.execute("SELECT * FROM schedules WHERE subject = ?", (override_subject,)).fetchone()
        conn.close()
        if row:
            return row["subject_code"], row["subject"], "PRESENT"
        return "", override_subject, "PRESENT"

    now = datetime.now()
    now_str = now.strftime("%H:%M")
    schedules = conn.execute(
        "SELECT * FROM schedules WHERE day_of_week = ? ORDER BY start_time ASC",
        (today_day,),
    ).fetchall()
    conn.close()

    for s in schedules:
        if s["start_time"] <= now_str <= s["end_time"]:
            start_dt = datetime.strptime(f"{now.strftime('%Y-%m-%d')} {s['start_time']}", "%Y-%m-%d %H:%M")
            status = "LATE" if now > (start_dt + timedelta(minutes=15)) else "PRESENT"
            return s["subject_code"], s["subject"], status

    return None, None, None


def get_ended_classes_with_missing_students():
    conn = get_db()
    now = datetime.now()
    now_str = now.strftime("%H:%M")
    today_day = now.strftime("%A")
    today_date = now.strftime("%Y-%m-%d")

    total_students = conn.execute("SELECT COUNT(*) FROM students").fetchone()[0]
    if total_students == 0:
        conn.close()
        return []

    ended_schedules = conn.execute(
        "SELECT * FROM schedules WHERE day_of_week = ? AND end_time < ? ORDER BY end_time ASC",
        (today_day, now_str),
    ).fetchall()

    pending = []
    for s in ended_schedules:
        logged_count = conn.execute(
            "SELECT COUNT(*) FROM attendance_logs WHERE subject = ? AND date_str = ?",
            (s["subject"], today_date),
        ).fetchone()[0]
        missing = total_students - logged_count
        if missing > 0:
            pending.append({
                "subject_code": s["subject_code"],
                "subject": s["subject"],
                "end_time": s["end_time"],
                "missing_count": missing,
            })

    conn.close()
    return pending