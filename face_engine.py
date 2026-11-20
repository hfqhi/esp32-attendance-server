import base64
import math
import os
import urllib.request
from datetime import datetime

import cv2
import numpy as np

from database import get_db

os.makedirs("models", exist_ok=True)
os.makedirs("enrolled_faces", exist_ok=True)
os.makedirs("captured_proofs", exist_ok=True)

YUNET_PATH = "models/face_detection_yunet_2023mar.onnx"
SFACE_PATH = "models/face_recognition_sface_2021dec.onnx"

MODELS = {
    YUNET_PATH: "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx",
    SFACE_PATH: "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx",
}

for path, url in MODELS.items():
    if not os.path.exists(path):
        print(f"Downloading {path}...")
        urllib.request.urlretrieve(url, path)

detector = cv2.FaceDetectorYN.create(YUNET_PATH, "", (320, 320), 0.72, 0.3, 5000)
recognizer = cv2.FaceRecognizerSF.create(SFACE_PATH, "")

COSINE_THRESHOLDS = {
    "esp32": 0.275,
    "webcam": 0.360,
}

TEXTURE_THRESHOLDS = {
    "esp32": 10.0,
    "webcam": 28.0,
}

# Centering & steadiness gates so glimpse/off-center faces are not captured prematurely
MIN_FACE_WIDTH_RATIO = 0.14
MAX_CENTER_OFFSET_X = 0.22
MAX_CENTER_OFFSET_Y = 0.22
MAX_STEADY_SHIFT = 0.045
REQUIRED_STEADY_FRAMES = 2

recognition_state = {
    "unknown_streak": 0,
    "steady_student_id": None,
    "steady_count": 0,
    "last_center": None,
}


def reset_steady_state():
    recognition_state["steady_student_id"] = None
    recognition_state["steady_count"] = 0
    recognition_state["last_center"] = None


def sanitize_slug(text, fallback="General"):
    clean = "_".join(str(text).strip().split())
    clean = "".join(c for c in clean if c.isalnum() or c in ("-", "_"))
    return clean or fallback


def decode_image(data_url_or_bytes):
    if isinstance(data_url_or_bytes, str):
        if "," in data_url_or_bytes:
            data_url_or_bytes = data_url_or_bytes.split(",")[1]
        img_bytes = base64.b64decode(data_url_or_bytes)
    else:
        img_bytes = data_url_or_bytes
    np_arr = np.frombuffer(img_bytes, np.uint8)
    return cv2.imdecode(np_arr, cv2.IMREAD_COLOR)


def enhance_indoor_frame(frame, source="webcam"):
    """Passes raw OV2640 CameraWebServer-profile frames untouched to preserve crisp contrast."""
    return frame


def save_reference_portrait(image_data, student_id, full_name):
    """Saves a clean JPEG portrait in enrolled_faces/ for ESP32-CAM SD syncing."""
    frame = decode_image(image_data)
    if frame is None:
        return None
    safe_id = sanitize_slug(student_id, "ID")
    safe_name = sanitize_slug(full_name, "Student")
    filename = f"{safe_id}_{safe_name}.jpg"
    cv2.imwrite(os.path.join("enrolled_faces", filename), frame, [cv2.IMWRITE_JPEG_QUALITY, 90])
    return filename


def save_proof_snapshot(frame, student_id, student_name, subject_code, status="PRESENT"):
    """
    Saves attendance capture using unified naming:
    Local: captured_proofs/<Day>/<Date>/<SubjectCode>/<Date>_<Time>_<StudentID>_<StudentName>_<Status>.jpg
    Returns (local_rel_to_captured_proofs, sd_target_path)
    """
    if frame is None:
        return "", ""
    now = datetime.now()
    day_slug = sanitize_slug(now.strftime("%A"), "Day")
    date_iso = now.strftime("%Y-%m-%d")
    time_slug = now.strftime("%I-%M-%S%p")

    subj_slug = sanitize_slug(subject_code or "General", "General")
    id_slug = sanitize_slug(student_id or "ID", "ID")
    stu_slug = sanitize_slug(student_name or "Student", "Student")
    stat_slug = sanitize_slug(status or "PRESENT", "PRESENT")

    fname = f"{date_iso}_{time_slug}_{id_slug}_{stu_slug}_{stat_slug}.jpg"
    rel_dir = os.path.join(day_slug, date_iso, subj_slug)

    abs_dir = os.path.join("captured_proofs", rel_dir)
    os.makedirs(abs_dir, exist_ok=True)

    abs_path = os.path.join(abs_dir, fname)
    cv2.imwrite(abs_path, frame, [cv2.IMWRITE_JPEG_QUALITY, 90])

    local_rel = os.path.join(rel_dir, fname).replace("\\", "/")
    sd_target = f"/{day_slug}/{subj_slug}/capture_log/{fname}"
    return local_rel, sd_target


def get_enrolled_embeddings():
    conn = get_db()
    students = conn.execute("SELECT * FROM students").fetchall()
    conn.close()
    records = []
    for s in students:
        emb_path = os.path.join("enrolled_faces", s["embedding_file"])
        if os.path.exists(emb_path):
            records.append({
                "student_id": s["student_id"],
                "full_name": s["full_name"],
                "section": s["section"],
                "embedding": np.load(emb_path),
            })
    return records


def extract_face_feature(image_data, exclude_student_id=None, source="webcam"):
    raw_frame = decode_image(image_data)
    if raw_frame is None:
        return None, "Invalid image data."

    frame = enhance_indoor_frame(raw_frame, source=source)
    h, w, _ = frame.shape
    detector.setInputSize((w, h))
    _, faces = detector.detect(frame)

    if faces is None or len(faces) == 0:
        return None, "No face detected. Ensure clear lighting and face the camera."

    if len(faces) > 1:
        return None, "Multiple faces detected. Only one person should be in frame."

    aligned = recognizer.alignCrop(frame, faces[0])
    feature = recognizer.feature(aligned)
    cosine_thresh = COSINE_THRESHOLDS.get(source, COSINE_THRESHOLDS["webcam"])

    for stu in get_enrolled_embeddings():
        if exclude_student_id and stu["student_id"].lower() == exclude_student_id.lower():
            continue
        score = recognizer.match(feature, stu["embedding"], cv2.FaceRecognizerSF_FR_COSINE)
        if score >= cosine_thresh:
            return None, f"Duplicate face detected: already registered as {stu['full_name']} ({stu['student_id']})."

    return feature, None


def compute_face_texture(frame, face):
    x, y, w, h = map(int, face[:4])
    x, y = max(0, x), max(0, y)
    crop = frame[y:y+h, x:x+w]
    if crop.size == 0:
        return 0.0
    normalized_crop = cv2.resize(crop, (112, 112), interpolation=cv2.INTER_AREA)
    gray = cv2.cvtColor(normalized_crop, cv2.COLOR_BGR2GRAY)
    return float(cv2.Laplacian(gray, cv2.CV_64F).var())


def evaluate_face_framing(frame, face):
    fh, fw, _ = frame.shape
    x, y, w, h = map(float, face[:4])
    width_ratio = w / max(1.0, float(fw))
    cx = (x + w * 0.5) / max(1.0, float(fw))
    cy = (y + h * 0.5) / max(1.0, float(fh))

    if width_ratio < MIN_FACE_WIDTH_RATIO:
        return False, "Move Closer to Camera", (cx, cy)

    if abs(cx - 0.5) > MAX_CENTER_OFFSET_X or abs(cy - 0.5) > MAX_CENTER_OFFSET_Y:
        return False, "Center Face in Frame", (cx, cy)

    return True, "Centered", (cx, cy)


def match_best_student(frame, source="webcam"):
    h, w, _ = frame.shape
    detector.setInputSize((w, h))
    _, faces = detector.detect(frame)

    if faces is None or len(faces) == 0:
        return None, None

    face = max(faces, key=lambda f: f[2] * f[3])
    aligned = recognizer.alignCrop(frame, face)
    feature = recognizer.feature(aligned)
    cosine_thresh = COSINE_THRESHOLDS.get(source, COSINE_THRESHOLDS["webcam"])

    best_student = None
    best_score = 0.0
    for stu in get_enrolled_embeddings():
        score = recognizer.match(feature, stu["embedding"], cv2.FaceRecognizerSF_FR_COSINE)
        if score > best_score:
            best_score = score
            if score >= cosine_thresh:
                best_student = stu

    return face, best_student


def identify_offline_face(image_bytes):
    raw_frame = decode_image(image_bytes)
    if raw_frame is None:
        return None, None
    frame = enhance_indoor_frame(raw_frame, source="esp32")
    face, best_student = match_best_student(frame, source="esp32")
    if face is None or best_student is None:
        return None, raw_frame
    is_centered, _, _ = evaluate_face_framing(frame, face)
    if not is_centered:
        return None, raw_frame
    return best_student, raw_frame


def verify_instant_presence(image_data, source="webcam"):
    raw_frame = decode_image(image_data)
    if raw_frame is None:
        reset_steady_state()
        return {"status": "ERROR", "message": "Invalid camera frame"}, None, None

    frame = enhance_indoor_frame(raw_frame, source=source)
    face, best_student = match_best_student(frame, source=source)

    if face is None:
        recognition_state["unknown_streak"] = 0
        reset_steady_state()
        return {"status": "NO_FACE", "message": "Ready to Scan"}, None, None

    is_centered, framing_msg, center_xy = evaluate_face_framing(frame, face)
    if not is_centered:
        recognition_state["unknown_streak"] = 0
        reset_steady_state()
        return {"status": "NO_FACE", "message": framing_msg}, None, None

    if not best_student:
        reset_steady_state()
        recognition_state["unknown_streak"] += 1
        if recognition_state["unknown_streak"] < 2:
            return {"status": "NO_FACE", "message": "Hold Steady — Scanning..."}, None, None
        return {"status": "UNKNOWN", "message": "Unregistered Face — Access Denied"}, None, None

    recognition_state["unknown_streak"] = 0

    min_texture = TEXTURE_THRESHOLDS.get(source, TEXTURE_THRESHOLDS["webcam"])
    texture = compute_face_texture(frame, face)
    if texture < min_texture:
        reset_steady_state()
        return {
            "status": "SPOOF",
            "message": "Flat Photo / Screen Blocked"
        }, None, None

    prev_id = recognition_state["steady_student_id"]
    prev_xy = recognition_state["last_center"]
    if prev_id == best_student["student_id"] and prev_xy is not None:
        shift = math.hypot(center_xy[0] - prev_xy[0], center_xy[1] - prev_xy[1])
        if shift <= MAX_STEADY_SHIFT:
            recognition_state["steady_count"] += 1
        else:
            recognition_state["steady_count"] = 1
    else:
        recognition_state["steady_student_id"] = best_student["student_id"]
        recognition_state["steady_count"] = 1

    recognition_state["last_center"] = center_xy

    if recognition_state["steady_count"] < REQUIRED_STEADY_FRAMES:
        return {
            "status": "NO_FACE",
            "message": f"Hold Steady ({best_student['full_name']})..."
        }, None, None

    reset_steady_state()
    return None, best_student, raw_frame