# ES-SEE Attendance System

Facial recognition attendance system for **CPE-3B** combining an **ESP32-CAM (OV2640)** hardware node with a **Python / Flask / OpenCV** server and browser dashboard.

## How It Works

1. **Camera Capture & Orientation (`esp32_cam_firmware.ino`):**
   - The ESP32-CAM initializes the OV2640 sensor at `640×480` (`VGA`) with `hmirror = 0` and `vflip = 0` so the image is upright and un-mirrored.
   - It streams live MJPEG video on `:81/stream`, serves single snapshots on `:80/capture`, and sends periodic heartbeats to `/api/esp32/register`.
2. **Face Detection & Recognition (`face_engine.py`):**
   - Incoming frames from either a browser webcam or the ESP32-CAM are decoded via OpenCV (`cv2`).
   - **YuNet** (`face_detection_yunet_2023mar.onnx`) locates faces in the frame, **SFace** (`face_recognition_sface_2021dec.onnx`) computes facial embeddings and compares cosine similarity against enrolled `.npy` profiles in `enrolled_faces/`, and a Laplacian texture check guards against flat photos/screens.
3. **Schedule & Attendance Logging (`database.py`, `app.py`):**
   - `/api/verify_frame` checks `get_active_schedule()` for the current day and time.
   - When a valid class is active, matching students are logged as `PRESENT` or `LATE` (with automatic `ABSENT` marking for ended classes) and a proof image is saved under `captured_proofs/`.
   - If the ESP32-CAM is offline or the server cannot be reached, frames are buffered on the MicroSD card in `/offline_buffer/` and synced via `/api/esp32/sync_offline` once reconnected.
4. **Web Dashboard (`templates/index.html`, `static/js/`):**
   - **Live Scanner:** View the webcam or ESP32-CAM feed (`4:3` frame), toggle scanning, inspect the live status banner, and browse **Recent Scans**.
   - **Attendance Logs:** Switch between **Table** and **Calendar** views, filter by date/student/subject, view proof snapshots, delete entries, or export CSV.
   - **Enrolled Faces:** Register or edit students via camera capture or image upload, and view student portrait thumbnails in the **Registered Students** table.
   - **Schedules:** Add, edit, filter by day, or remove weekly class schedules.

## Quick Start

### 1. Configure & Flash ESP32-CAM

1. Open `esp32_cam_firmware/esp32_cam_firmware.ino` in Arduino IDE.
2. Set `WIFI_SSID`, `WIFI_PASSWORD`, and `SERVER_BASE` (your host machine's local IPv4 address on port `5000`).
3. Select **AI Thinker ESP32-CAM** and flash the firmware, then press **RST**.

### 2. Run the Flask Server

```bash
python app.py
```
