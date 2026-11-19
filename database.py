import sqlite3
from datetime import datetime

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

    sched_cols = [row["name"] for row in cur.execute("PRAGMA table_info(schedules)").fetchall()]
    if "day_of_week" not in sched_cols:
        cur.execute("ALTER TABLE schedules ADD COLUMN day_of_week TEXT NOT NULL DEFAULT 'Monday'")
    if "subject_code" not in sched_cols:
        cur.execute("ALTER TABLE schedules ADD COLUMN subject_code TEXT NOT NULL DEFAULT ''")

    cur.execute("""
        CREATE TABLE IF NOT EXISTS attendance_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            student_id TEXT NOT NULL,
            full_name TEXT NOT NULL,
            subject_code TEXT NOT NULL DEFAULT '',
            subject TEXT NOT NULL,
            status TEXT NOT NULL,
            date_str TEXT NOT NULL,
            time_str TEXT NOT NULL,
            UNIQUE(student_id, subject, date_str)
        )
    """)

    log_cols = [row["name"] for row in cur.execute("PRAGMA table_info(attendance_logs)").fetchall()]
    if "subject_code" not in log_cols:
        cur.execute("ALTER TABLE attendance_logs ADD COLUMN subject_code TEXT NOT NULL DEFAULT ''")

    cur.execute("SELECT COUNT(*) FROM schedules")
    if cur.fetchone()[0] == 0:
        default_schedules = [
            ("Monday", "CPE 311", "Logic Circuits", "07:00", "10:00"),
            ("Monday", "CPE 312", "Mixed Signals", "10:00", "11:30"),
            ("Monday", "MATH 311", "Discrete Math", "13:00", "14:30"),
            ("Monday", "ELEC 311", "Cognate", "14:30", "16:00"),
        ]
        cur.executemany(
            "INSERT INTO schedules (day_of_week, subject_code, subject, start_time, end_time) VALUES (?, ?, ?, ?, ?)",
            default_schedules,
        )

    conn.commit()
    conn.close()

def to_minutes(time_24):
    h, m = map(int, time_24.split(":"))
    return h * 60 + m

def format_12h(time_24):
    h, m = map(int, time_24.split(":"))
    period = "PM" if h >= 12 else "AM"
    h12 = h % 12 or 12
    return f"{h12}:{m:02d} {period}"

def get_active_schedule(override_subject=None):
    conn = get_db()
    schedules = conn.execute("SELECT * FROM schedules").fetchall()
    conn.close()

    now = datetime.now()
    today_name = now.strftime("%A")
    now_mins = now.hour * 60 + now.minute

    if override_subject:
        for sch in schedules:
            if sch["subject"] == override_subject:
                return sch["subject_code"], sch["subject"], "PRESENT"

    for sch in schedules:
        if sch["day_of_week"].lower() != today_name.lower():
            continue
        start_mins = to_minutes(sch["start_time"])
        end_mins = to_minutes(sch["end_time"])
        if start_mins <= now_mins < end_mins:
            status = "PRESENT" if (now_mins - start_mins) <= 15 else "LATE"
            return sch["subject_code"], sch["subject"], status

    return None, None, None

def get_ended_classes_with_missing_students():
    conn = get_db()
    now = datetime.now()
    today_name = now.strftime("%A")
    date_str = now.strftime("%Y-%m-%d")
    now_mins = now.hour * 60 + now.minute

    total_students = conn.execute("SELECT COUNT(*) FROM students").fetchone()[0]
    if total_students == 0:
        conn.close()
        return []

    today_schedules = conn.execute(
        "SELECT * FROM schedules WHERE LOWER(day_of_week) = LOWER(?) ORDER BY start_time ASC",
        (today_name,),
    ).fetchall()

    pending = []
    for sch in today_schedules:
        if now_mins >= to_minutes(sch["end_time"]):
            logged_count = conn.execute(
                "SELECT COUNT(DISTINCT student_id) FROM attendance_logs WHERE subject = ? AND date_str = ?",
                (sch["subject"], date_str),
            ).fetchone()[0]
            missing = total_students - logged_count
            if missing > 0:
                pending.append({
                    "subject_code": sch["subject_code"],
                    "subject": sch["subject"],
                    "missing_count": missing,
                })
    conn.close()
    return pending

def validate_schedule_input(conn, day_of_week, subject_code, subject, start_time, end_time, exclude_id=None):
    if not day_of_week or not subject_code or not subject or not start_time or not end_time:
        return "Day, Subject Code, Subject Name, Start, and End times are required."

    new_start = to_minutes(start_time)
    new_end = to_minutes(end_time)

    if new_end <= new_start:
        return "End time must be later than start time."

    if exclude_id is not None:
        day_schedules = conn.execute(
            "SELECT * FROM schedules WHERE LOWER(day_of_week) = LOWER(?) AND id != ?",
            (day_of_week, exclude_id),
        ).fetchall()
    else:
        day_schedules = conn.execute(
            "SELECT * FROM schedules WHERE LOWER(day_of_week) = LOWER(?)",
            (day_of_week,),
        ).fetchall()

    for sch in day_schedules:
        if sch["subject_code"].lower() == subject_code.lower():
            return f"Code '{subject_code}' already exists on {day_of_week}."
        if sch["subject"].lower() == subject.lower():
            return f"'{subject}' already exists on {day_of_week}."

        ex_start = to_minutes(sch["start_time"])
        ex_end = to_minutes(sch["end_time"])
        if new_start < ex_end and new_end > ex_start:
            return f"Time overlaps with {sch['subject_code']} ({format_12h(sch['start_time'])} - {format_12h(sch['end_time'])})."

    return None