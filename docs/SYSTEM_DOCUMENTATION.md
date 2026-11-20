# ES-SEE Attendance System — Technical Documentation

**System:** ES-SEE Facial Recognition Attendance System (`esp32-attendance-server`)
**Section:** CPE-3B
**Architecture:** ESP32-CAM (OV2640 + MicroSD) + Python/Flask Backend + OpenCV (`cv2` YuNet & SFace) + Web Dashboard

---

## 1. Current Version Summary

This release establishes the current baseline for the ES-SEE Attendance System:

1. **ESP32-CAM Firmware (`esp32_cam_firmware/esp32_cam_firmware.ino`):**
   - Initializes the OV2640 sensor at `20 MHz` XCLK with PSRAM (`FRAMESIZE_UXGA` -> `FRAMESIZE_VGA` at `640×480`, `jpeg_quality = 10`, `fb_count = 2`, `CAMERA_GRAB_LATEST`) and `hmirror = 0`, `vflip = 0` for upright, non-inverted orientation.
   - Serves MJPEG live video on port `81` (`/stream`) and single JPEG snapshots on port `80` (`/capture`).
   - Registers its IP address with the Flask server (`/api/esp32/register`), syncs NTP time (`UTC+8`), syncs enrolled portraits to `/enrolled_faces/`, saves structured attendance captures under `/attendance_captures/<Day>/<Date>/<SubjectCode>/`, writes structured CSV logs under `/logs/<Day>/<Date>/`, and buffers frames in `/offline_buffer/` when Wi-Fi or the server is unreachable.
2. **Backend & Vision Pipeline (`app.py`, `face_engine.py`, `database.py`):**
   - Uses OpenCV `FaceDetectorYN` (YuNet) for face detection and `FaceRecognizerSF` (SFace) with cosine similarity matching and Laplacian texture anti-spoofing.
   - Resolves enrolled student portrait URLs (`/api/esp32/portraits/<filename>`) for `/api/dashboard_data`.
   - Enforces active class schedule matching (`PRESENT`, `LATE`, `ABSENT`, or `NO_CLASS`) without a manual schedule override dropdown.
3. **Web Dashboard (`templates/index.html`, `static/js/dashboard.js`, `static/js/camera.js`):**
   - Displays the live camera feed in a `4:3` viewport (`aspect-[4/3]`) alongside **Recent Scans**, **Attendance Logs** (Table and Calendar views + CSV export), **Enrolled Faces** (with student portrait thumbnails and preview modal), and day-grouped **Schedules**.

---

## 2. API & Endpoint Reference

| Method                    | Endpoint                                 | Purpose                                                                              |
| :------------------------ | :--------------------------------------- | :----------------------------------------------------------------------------------- |
| `GET`                     | `/`                                      | Renders the web dashboard (`templates/index.html`)                                   |
| `POST`                    | `/api/esp32/register`                    | Records the ESP32-CAM IP address and heartbeat timestamp                             |
| `GET`                     | `/api/esp32/proxy_capture`               | Proxies a single JPEG snapshot from the online ESP32-CAM                             |
| `GET`                     | `/api/esp32/enrolled_portraits`          | Lists enrolled `.jpg` portraits for ESP32-CAM SD card sync                           |
| `GET`                     | `/api/esp32/portraits/<filename>`        | Serves an enrolled student portrait from `enrolled_faces/`                           |
| `POST`                    | `/api/esp32/sync_offline`                | Processes buffered offline JPEG frames from the ESP32-CAM                            |
| `GET`                     | `/api/dashboard_data`                    | Returns students (with `photo_url`), schedules, logs, active class, and ESP32 status |
| `POST`                    | `/api/enroll`                            | Enrolls a student, saves `.npy` face embedding, and saves reference portrait         |
| `PUT` / `DELETE`          | `/api/students/<student_id>`             | Updates or deletes an enrolled student and associated files                          |
| `POST` / `PUT` / `DELETE` | `/api/schedules` / `/api/schedules/<id>` | Creates, updates, or deletes class schedules                                         |
| `POST`                    | `/api/verify_frame`                      | Verifies a webcam or ESP32-CAM frame and records attendance against active schedule  |
| `DELETE`                  | `/api/logs/<log_id>` / `/api/logs/clear` | Deletes a single attendance log or clears logs                                       |
| `GET`                     | `/api/export_csv`                        | Exports filtered attendance logs as a UTF-8 CSV download                             |

---

## 3. SD Card & Server Directory Layout

### ESP32-CAM MicroSD (`/sdcard`)

- `/enrolled_faces/` — Synced student reference portraits
- `/attendance_captures/<Day>/<Date>/<SubjectCode>/` — Captured attendance photos
- `/logs/<Day>/<Date>/<SubjectCode>_attendance.csv` — Per-subject CSV attendance logs
- `/offline_buffer/` — Temporary JPEG buffer when offline

### Server Workspace

- `enrolled_faces/` — Stored `.npy` embeddings and reference `.jpg` portraits
- `captured_proofs/<Day>/<Date>/<SubjectCode>/` — Attendance verification proof snapshots
- `models/` — ONNX weights for YuNet and SFace
- `templates/` & `static/` — Dashboard HTML, CSS, and JavaScript files
