import base64
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

# 0.72 detection score threshold prevents missed detections on OV2640 indoor frames
detector = cv2.FaceDetectorYN.create(YUNET_PATH, "", (320, 320), 0.72, 0.3, 5000)
recognizer = cv2.FaceRecognizerSF.create(SFACE_PATH, "")

COSINE_THRESHOLDS = {
    "esp32": 0.265,
    "webcam": 0.360,
}

# Passive anti-spoof Laplacian sharpness threshold (detects flat phone screens / printed photos)
TEXTURE_THRESHOLDS = {
    "esp32": 9.5,
    "webcam": 28.0,
}

recognition_state = {
    "unknown_streak": 0,
    "last_spoof_ts": 0.0,
}


def sanitize_slug(text):
    clean = "_".join(str(text).strip().split())
    clean = "".join(c for c in clean if c.isalnum() or c in ("-", "_"))
    return clean or "General"


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
    """Normalizes indoor OV2640 luminance gently while preserving facial edges."""
    if frame is None or source != "esp32":
        return frame

    lab = cv2.cvtColor(frame, cv2.COLOR_BGR2LAB)
    l_chan, a_chan, b_chan = cv2.split(lab)
    clahe = cv2.createCLAHE(clipLimit=2.2, tileGridSize=(8, 8))
    l_eq = clahe.apply(l_chan)
    return cv2.cvtColor(cv2.merge((l_eq, a_chan, b_chan)), cv2.COLOR_LAB2BGR)


def save_reference_portrait(image_data, student_id, full_name):
    """Saves a clean JPEG portrait in enrolled_faces/ for ESP32-CAM SD syncing."""
    frame = decode_image(image_data)
    if frame is None:
        return None
    safe_id = sanitize_slug(student_id)
    safe_name = sanitize_slug(full_name)
    filename = f"{safe_id}_{safe_name}.jpg"
    cv2.imwrite(os.path.join("enrolled_faces", filename), frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
    return filename


def save_proof_snapshot(frame, student_name, subject_code, is_spoof=False):
    """Saves attendance or spoof proof sorted by Day / Date / SubjectCode."""
    if frame is None:
        return ""
    now = datetime.now()
    day_name = now.strftime("%A")
    date_iso = now.strftime("%Y-%m-%d")
    time_slug = now.strftime("%I-%M-%S%p")

    if is_spoof:
        rel_dir = os.path.join("spoof_alerts", day_name, date_iso)
    else:
        subj_slug = sanitize_slug(subject_code or "General")
        rel_dir = os.path.join(day_name, date_iso, subj_slug)

    abs_dir = os.path.join("captured_proofs", rel_dir)
    os.makedirs(abs_dir, exist_ok=True)

    stu_slug = sanitize_slug(student_name)
    subj_tag = sanitize_slug(subject_code or "ATT")
    fname = f"{stu_slug}_{subj_tag}_{date_iso}_{time_slug}.jpg"
    abs_path = os.path.join(abs_dir, fname)
    cv2.imwrite(abs_path, frame, [cv2.IMWRITE_JPEG_QUALITY, 88])
    return os.path.join(rel_dir, fname).replace("\\", "/")


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


def match_best_student(frame, source="webcam"):
    h, w, _ = frame.shape
    detector.setInputSize((w, h))
    _, faces = detector.detect(frame)

    if faces is None or len(faces) == 0:
        return None, None

    # Pick the largest face in frame
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
    """Matches a buffered offline JPEG against enrolled students."""
    raw_frame = decode_image(image_bytes)
    if raw_frame is None:
        return None, None
    frame = enhance_indoor_frame(raw_frame, source="esp32")
    _, best_student = match_best_student(frame, source="esp32")
    return best_student, raw_frame


def verify_instant_presence(image_data, source="webcam"):
    """Instant single-frame recognition with passive photo-spoof detection."""
    raw_frame = decode_image(image_data)
    if raw_frame is None:
        return {"status": "ERROR", "message": "Invalid camera frame"}, None, None

    frame = enhance_indoor_frame(raw_frame, source=source)
    face, best_student = match_best_student(frame, source=source)

    if face is None:
        recognition_state["unknown_streak"] = 0
        return {"status": "NO_FACE", "message": "Ready to Scan"}, None, None

    if not best_student:
        recognition_state["unknown_streak"] += 1
        if recognition_state["unknown_streak"] < 2:
            return {"status": "NO_FACE", "message": "Scanning Face..."}, None, None
        return {"status": "UNKNOWN", "message": "Unregistered Face — Access Denied"}, None, None

    recognition_state["unknown_streak"] = 0

    # Passive anti-spoof check: detect flat photo / phone screen replay
    min_texture = TEXTURE_THRESHOLDS.get(source, TEXTURE_THRESHOLDS["webcam"])
    texture = compute_face_texture(frame, face)
    if texture < min_texture:
        return {
            "status": "SPOOF",
            "student_name": best_student["full_name"],
            "texture_score": round(texture, 1),
            "message": f"Image/Screen Spoof Blocked ({best_student['full_name']})"
        }, None, raw_frame

    return None, best_student, raw_frame