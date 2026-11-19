import base64
import os
import random
import time
import urllib.request

import cv2
import numpy as np

from database import get_db

os.makedirs("models", exist_ok=True)
os.makedirs("enrolled_faces", exist_ok=True)

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

detector = cv2.FaceDetectorYN.create(YUNET_PATH, "", (320, 320), 0.9, 0.3, 5000)
recognizer = cv2.FaceRecognizerSF.create(SFACE_PATH, "")

COSINE_THRESHOLD = 0.363
MIN_TEXTURE_SCORE = 35.0

liveness_state = {
    "student_id": None,
    "challenge": None,
    "baseline_smile": 0.0,
    "baseline_yaw": 1.0,
    "timestamp": 0.0,
}

def decode_image(data_url_or_bytes):
    if isinstance(data_url_or_bytes, str):
        if "," in data_url_or_bytes:
            data_url_or_bytes = data_url_or_bytes.split(",")[1]
        img_bytes = base64.b64decode(data_url_or_bytes)
    else:
        img_bytes = data_url_or_bytes
    np_arr = np.frombuffer(img_bytes, np.uint8)
    return cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

def save_reference_portrait(image_data, student_id, full_name):
    """Saves a clean JPEG portrait in enrolled_faces/ for ESP32-CAM SD syncing."""
    frame = decode_image(image_data)
    if frame is None:
        return None
    safe_id = "".join(c for c in student_id if c.isalnum() or c in ("-", "_"))
    safe_name = "_".join(full_name.split())
    safe_name = "".join(c for c in safe_name if c.isalnum() or c in ("-", "_"))
    filename = f"{safe_id}_{safe_name}.jpg"
    cv2.imwrite(os.path.join("enrolled_faces", filename), frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
    return filename

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

def extract_face_feature(image_data, exclude_student_id=None):
    frame = decode_image(image_data)
    if frame is None:
        return None, "Invalid image data."

    h, w, _ = frame.shape
    detector.setInputSize((w, h))
    _, faces = detector.detect(frame)

    if faces is None or len(faces) == 0:
        return None, "No face detected. Ensure clear lighting and face the camera."

    if len(faces) > 1:
        return None, "Multiple faces detected. Only one person should be in frame."

    aligned = recognizer.alignCrop(frame, faces[0])
    feature = recognizer.feature(aligned)

    for stu in get_enrolled_embeddings():
        if exclude_student_id and stu["student_id"].lower() == exclude_student_id.lower():
            continue
        score = recognizer.match(feature, stu["embedding"], cv2.FaceRecognizerSF_FR_COSINE)
        if score >= COSINE_THRESHOLD:
            return None, f"Duplicate face detected: already registered as {stu['full_name']} ({stu['student_id']})."

    return feature, None

def analyze_liveness(frame, face):
    x, y, w, h = map(int, face[:4])
    x, y = max(0, x), max(0, y)
    crop = frame[y:y+h, x:x+w]
    texture = 0.0
    if crop.size > 0:
        gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
        texture = cv2.Laplacian(gray, cv2.CV_64F).var()

    re_x, le_x, nose_x, rm_x, lm_x = face[4], face[6], face[8], face[10], face[12]
    eye_dist = abs(le_x - re_x) + 1e-5
    smile_ratio = abs(lm_x - rm_x) / eye_dist
    yaw_ratio = abs(nose_x - re_x) / (abs(le_x - nose_x) + 1e-5)
    return texture, smile_ratio, yaw_ratio

def match_best_student(frame):
    h, w, _ = frame.shape
    detector.setInputSize((w, h))
    _, faces = detector.detect(frame)

    if faces is None or len(faces) == 0:
        return None, None

    face = faces[0]
    aligned = recognizer.alignCrop(frame, face)
    feature = recognizer.feature(aligned)

    best_student = None
    best_score = 0.0
    for stu in get_enrolled_embeddings():
        score = recognizer.match(feature, stu["embedding"], cv2.FaceRecognizerSF_FR_COSINE)
        if score > best_score:
            best_score = score
            if score >= COSINE_THRESHOLD:
                best_student = stu

    return face, best_student

def identify_offline_face(image_bytes):
    """Matches a single queued offline JPEG without requiring interactive challenge."""
    frame = decode_image(image_bytes)
    if frame is None:
        return None
    _, best_student = match_best_student(frame)
    return best_student

def verify_and_check_liveness(image_data):
    frame = decode_image(image_data)
    if frame is None:
        return {"status": "ERROR", "message": "Invalid camera frame"}, None

    face, best_student = match_best_student(frame)
    if face is None:
        liveness_state["student_id"] = None
        return {"status": "NO_FACE", "message": "Ready to Scan"}, None

    if not best_student:
        liveness_state["student_id"] = None
        return {"status": "UNKNOWN", "message": "Unregistered Face — Access Denied"}, None

    texture, smile_ratio, yaw_ratio = analyze_liveness(frame, face)
    if texture < MIN_TEXTURE_SCORE:
        return {"status": "SPOOF", "message": "Spoof Blocked — Low Texture"}, None

    now_ts = time.time()
    sid = best_student["student_id"]

    if liveness_state["student_id"] != sid or (now_ts - liveness_state["timestamp"] > 6.0):
        challenge = random.choice(["SMILE", "TURN_HEAD"])
        liveness_state.update({
            "student_id": sid,
            "challenge": challenge,
            "baseline_smile": smile_ratio,
            "baseline_yaw": yaw_ratio,
            "timestamp": now_ts,
        })
        prompt = "SMILE TO VERIFY" if challenge == "SMILE" else "TURN HEAD SLIGHTLY"
        return {
            "status": "CHALLENGE",
            "student_name": best_student["full_name"],
            "message": f"{best_student['full_name']} • {prompt}"
        }, None

    passed = False
    if liveness_state["challenge"] == "SMILE" and smile_ratio > liveness_state["baseline_smile"] * 1.10:
        passed = True
    elif liveness_state["challenge"] == "TURN_HEAD" and (
        yaw_ratio > liveness_state["baseline_yaw"] * 1.30 or yaw_ratio < liveness_state["baseline_yaw"] * 0.72
    ):
        passed = True

    if not passed:
        prompt = "SMILE TO VERIFY" if liveness_state["challenge"] == "SMILE" else "TURN HEAD SLIGHTLY"
        return {
            "status": "CHALLENGE",
            "student_name": best_student["full_name"],
            "message": f"{best_student['full_name']} • {prompt}"
        }, None

    liveness_state["student_id"] = None
    return None, best_student