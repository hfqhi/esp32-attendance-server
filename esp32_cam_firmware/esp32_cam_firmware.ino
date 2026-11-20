#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"

// ==========================================
// 1. NETWORK & SERVER CONFIGURATION
// ==========================================
const char* WIFI_SSID     = "CutieNet";
const char* WIFI_PASSWORD = "CutiePie@123";

// Replace 192.168.1.100 with your laptop's IPv4 address from `ipconfig`
const char* SERVER_BASE   = "http://192.168.1.5:5000";

// Philippine Standard Time (UTC+8)
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET    = 8 * 3600;
const int   DAYLIGHT_OFF  = 0;

// AI-Thinker ESP32-CAM Pin Definitions
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN      4
#define ONBOARD_LED_PIN   33

WebServer camHttpServer(80);
WiFiServer streamServer(81);
WiFiClient streamClient;

bool sdAvailable = false;
unsigned long lastCaptureMs = 0;
unsigned long lastHeartbeatMs = 0;
const unsigned long CAPTURE_INTERVAL_MS = 550;
const unsigned long HEARTBEAT_INTERVAL_MS = 8000;

uint8_t prevSample[64] = {0};
size_t prevFrameLen = 0;
RTC_DATA_ATTR uint32_t bufferCounter = 1;

// ==========================================
// 2. TIMESTAMP & SD DIRECTORY HELPERS
// ==========================================
struct TimeStamps {
  String dayName;    // Friday
  String dateIso;    // 2026-11-20
  String timeFile;   // 06-40-00PM
  String timeHuman;  // 06:40:00 PM
  bool validNtp;
};

void keepFlashGroundedOff() {
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
}

TimeStamps getCurrentTimestamps() {
  TimeStamps ts;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 40)) {
    char dDay[16], dIso[16], tFile[16], tHuman[16];
    strftime(dDay, sizeof(dDay), "%A", &timeinfo);
    strftime(dIso, sizeof(dIso), "%Y-%m-%d", &timeinfo);
    strftime(tFile, sizeof(tFile), "%I-%M-%S%p", &timeinfo);
    strftime(tHuman, sizeof(tHuman), "%I:%M:%S %p", &timeinfo);
    ts.dayName = String(dDay);
    ts.dateIso = String(dIso);
    ts.timeFile = String(tFile);
    ts.timeHuman = String(tHuman);
    ts.validNtp = true;
  } else {
    char idxBuf[16];
    snprintf(idxBuf, sizeof(idxBuf), "SEQ-%04lu", (unsigned long)bufferCounter++);
    ts.dayName = "Unsynced";
    ts.dateIso = "0000-00-00";
    ts.timeFile = String(idxBuf);
    ts.timeHuman = String(idxBuf);
    ts.validNtp = false;
  }
  return ts;
}

String sanitizeSlug(const String& raw, const String& fallback) {
  String clean = "";
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (isalnum(c) || c == '-' || c == '_') clean += c;
    else if (c == ' ') clean += '_';
  }
  return clean.length() > 0 ? clean : fallback;
}

void ensureDir(const String& path) {
  if (!SD_MMC.exists(path)) {
    SD_MMC.mkdir(path);
  }
}

String ensureCaptureLogDir(const String& dayName, const String& subjectCode) {
  String daySlug = sanitizeSlug(dayName, "Day");
  String subjSlug = sanitizeSlug(subjectCode, "General");

  String dayDir = "/" + daySlug;
  String subjDir = dayDir + "/" + subjSlug;
  String logDir = subjDir + "/capture_log";

  ensureDir(dayDir);
  ensureDir(subjDir);
  ensureDir(logDir);
  return logDir;
}

// ==========================================
// 3. PRESENCE / MOTION GATE
// ==========================================
bool hasSignificantMotion(camera_fb_t* fb) {
  if (!fb || fb->len < 1024) return false;

  long lenDiff = abs((long)fb->len - (long)prevFrameLen);
  bool sizeChanged = (prevFrameLen == 0) || (lenDiff > (long)(prevFrameLen * 0.08));

  uint8_t currSample[64];
  size_t usable = fb->len > 512 ? fb->len - 512 : fb->len;
  size_t step = usable / 64;
  if (step == 0) step = 1;

  int diffCount = 0;
  for (int i = 0; i < 64; i++) {
    size_t idx = 256 + i * step;
    if (idx >= fb->len) idx = fb->len - 1;
    currSample[i] = fb->buf[idx];
    if (abs((int)currSample[i] - (int)prevSample[i]) > 18) {
      diffCount++;
    }
    prevSample[i] = currSample[i];
  }
  prevFrameLen = fb->len;

  return sizeChanged || (diffCount >= 18);
}

// ==========================================
// 4. CAMERA INITIALIZATION (PRESERVED LIGHTING + UPRIGHT VGA)
// ==========================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_UXGA;
  config.jpeg_quality = 10;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.fb_count     = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    if (s->id.PID == OV3660_PID) {
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
  }

  for (int i = 0; i < 6; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(35);
  }

  return true;
}

bool initMicroSD() {
  // 1-bit SD mode leaves GPIO 4 free; re-assert GPIO 4 LOW immediately after mount
  if (!SD_MMC.begin("/sdcard", true)) {
    keepFlashGroundedOff();
    Serial.println("[SD] Mount failed. Verify FAT32 format.");
    return false;
  }
  keepFlashGroundedOff();

  if (SD_MMC.cardType() == CARD_NONE) return false;

  ensureDir("/enrolled_faces");
  ensureDir("/offline_buffer");
  Serial.println("[SD] Ready (/enrolled_faces & /<Day>/<SubjectCode>/capture_log).");
  return true;
}

// ==========================================
// 5. STRUCTURED SD DATABASE STORAGE
// ==========================================
String saveCapturePhotoAndLogToSd(
  camera_fb_t* fb,
  const String& studentId,
  const String& studentName,
  const String& subjectCode,
  const String& status,
  const TimeStamps& ts
) {
  if (!sdAvailable || !fb) return "";

  String logDir   = ensureCaptureLogDir(ts.dayName, subjectCode);
  String dateSlug = sanitizeSlug(ts.dateIso, "0000-00-00");
  String subjSlug = sanitizeSlug(subjectCode, "General");
  String idSlug   = sanitizeSlug(studentId, "ID");
  String stuSlug  = sanitizeSlug(studentName, "Student");
  String statSlug = sanitizeSlug(status, "PRESENT");

  // Optimized filename: <Date>_<Time>_<StudentID>_<StudentName>_<Status>.jpg
  String photoFileName = dateSlug + "_" + ts.timeFile + "_" + idSlug + "_" + stuSlug + "_" + statSlug + ".jpg";
  String photoFullPath = logDir + "/" + photoFileName;

  File img = SD_MMC.open(photoFullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
    Serial.printf("[SD-DB] Saved capture: %s\n", photoFullPath.c_str());
  } else {
    photoFullPath = "";
  }

  // Per-day, per-subject CSV capture log inside /<Day>/<SubjectCode>/capture_log/
  String csvPath = logDir + "/" + dateSlug + "_attendance.csv";
  bool isNew = !SD_MMC.exists(csvPath);

  File csvFile = SD_MMC.open(csvPath, isNew ? FILE_WRITE : FILE_APPEND);
  if (csvFile) {
    if (isNew) {
      csvFile.println("Student ID,Student Name,Subject Code,Status,Day,Date,Time,Capture File");
    }
    csvFile.printf("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
                   idSlug.c_str(),
                   studentName.c_str(),
                   subjSlug.c_str(),
                   statSlug.c_str(),
                   ts.dayName.c_str(),
                   ts.dateIso.c_str(),
                   ts.timeHuman.c_str(),
                   photoFileName.c_str());
    csvFile.close();
  }

  keepFlashGroundedOff();
  return photoFullPath;
}

void bufferFrameSilentlyWhenOffline(camera_fb_t* fb, const TimeStamps& ts) {
  if (!sdAvailable || !fb) return;
  File root = SD_MMC.open("/offline_buffer");
  int count = 0;
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) { count++; f.close(); f = root.openNextFile(); }
    root.close();
  }
  if (count >= 60) return;

  String fullPath = "/offline_buffer/BUF_" + ts.dateIso + "_" + ts.timeFile + ".jpg";
  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
  }
  keepFlashGroundedOff();
}

// ==========================================
// 6. SERVER SYNC: HEARTBEAT, ENROLLED PORTRAITS & OFFLINE BUFFER
// ==========================================
void sendHeartbeatToServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);

  String body = "{\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  http.POST(body);
  http.end();
}

void syncEnrolledPortraitsFromServer() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/enrolled_portraits");
  http.setTimeout(3000);

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray arr = doc["portraits"].as<JsonArray>();
      for (JsonVariant v : arr) {
        String fname = v.as<String>();
        String sdPath = "/enrolled_faces/" + fname;
        if (!SD_MMC.exists(sdPath)) {
          HTTPClient dl;
          dl.begin(String(SERVER_BASE) + "/api/esp32/portraits/" + fname);
          if (dl.GET() == 200) {
            File out = SD_MMC.open(sdPath, FILE_WRITE);
            if (out) {
              dl.writeToStream(&out);
              out.close();
            }
          }
          dl.end();
        }
      }
    }
  }
  http.end();
  keepFlashGroundedOff();
}

void syncOfflineBufferToServer() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;
  File root = SD_MMC.open("/offline_buffer");
  if (!root || !root.isDirectory()) return;

  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String entryPath = String(entry.path());
      size_t fsize = entry.size();
      uint8_t* buf = (uint8_t*)ps_malloc(fsize);
      if (!buf) buf = (uint8_t*)malloc(fsize);

      if (buf && fsize > 0) {
        entry.read(buf, fsize);
        entry.close();

        HTTPClient http;
        http.begin(String(SERVER_BASE) + "/api/esp32/sync_offline");
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("X-Camera-Source", "esp32");
        http.setTimeout(3500);

        int code = http.POST(buf, fsize);
        if (code == 200) {
          JsonDocument doc;
          if (!deserializeJson(doc, http.getString())) {
            const char* status   = doc["status"] | "UNKNOWN";
            const char* stuId    = doc["student_id"] | "ID";
            const char* stuName  = doc["student_name"] | "";
            const char* subjCode = doc["subject_code"] | "General";
            const char* attStat  = doc["attendance_status"] | "PRESENT";
            TimeStamps ts = getCurrentTimestamps();

            if (strcmp(status, "LOGGED") == 0 && strlen(stuName) > 0) {
              camera_fb_t fakeFb;
              fakeFb.buf = buf;
              fakeFb.len = fsize;
              saveCapturePhotoAndLogToSd(&fakeFb, stuId, stuName, subjCode, attStat, ts);
            }
            SD_MMC.remove(entryPath);
          }
        }
        http.end();
        free(buf);
      } else {
        entry.close();
      }
    } else {
      entry.close();
    }
    entry = root.openNextFile();
  }
  root.close();
  keepFlashGroundedOff();
}

// ==========================================
// 7. HIGH-SPEED WEB DASHBOARD STREAMING ENDPOINTS
// ==========================================
void handleSingleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    camHttpServer.send(503, "text/plain", "Camera capture failed");
    return;
  }
  camHttpServer.sendHeader("Access-Control-Allow-Origin", "*");
  camHttpServer.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void serviceMjpegStream() {
  if (!streamClient || !streamClient.connected()) {
    streamClient = streamServer.available();
    if (streamClient) {
      streamClient.setNoDelay(true);
      streamClient.println("HTTP/1.1 200 OK");
      streamClient.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
      streamClient.println("Access-Control-Allow-Origin: *");
      streamClient.println();
    }
  }

  if (streamClient && streamClient.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      streamClient.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      streamClient.write(fb->buf, fb->len);
      streamClient.print("\r\n");
      esp_camera_fb_return(fb);
    }
  }
}

// ==========================================
// 8. SETUP & MAIN LOOP
// ==========================================
void setup() {
  keepFlashGroundedOff();
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== CPE-3B ESP32-CAM v2 Node ===");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed.");
    while (true) delay(1000);
  }

  sdAvailable = initMicroSD();
  keepFlashGroundedOff();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(400);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Online at http://%s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, DAYLIGHT_OFF, NTP_SERVER);

    camHttpServer.on("/capture", HTTP_GET, handleSingleCapture);
    camHttpServer.begin();
    streamServer.begin();

    sendHeartbeatToServer();
    syncEnrolledPortraitsFromServer();
    syncOfflineBufferToServer();
  } else {
    Serial.println("\n[WIFI] Offline — buffering frames until reconnect");
  }
  keepFlashGroundedOff();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    camHttpServer.handleClient();
    serviceMjpegStream();
    yield();

    if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = millis();
      sendHeartbeatToServer();
    }
  }

  if (millis() - lastCaptureMs < CAPTURE_INTERVAL_MS) return;
  lastCaptureMs = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  if (!hasSignificantMotion(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  TimeStamps ts = getCurrentTimestamps();

  if (WiFi.status() != WL_CONNECTED) {
    bufferFrameSilentlyWhenOffline(fb, ts);
    esp_camera_fb_return(fb);
    return;
  }

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/verify_frame");
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Camera-Source", "esp32");
  http.setTimeout(2500);

  int httpCode = http.POST(fb->buf, fb->len);
  if (httpCode == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      const char* status   = doc["status"]  | "UNKNOWN";
      const char* message  = doc["message"] | "";
      const char* stuId    = doc["student_id"] | "ID";
      const char* stuName  = doc["student_name"] | "";
      const char* subjCode = doc["subject_code"] | "General";
      const char* attStat  = doc["attendance_status"] | "PRESENT";

      Serial.printf("[%s] %s\n", status, message);

      if ((strcmp(status, "LOGGED") == 0 || strcmp(status, "ALREADY_LOGGED") == 0) && strlen(stuName) > 0) {
        if (strcmp(status, "LOGGED") == 0) {
          saveCapturePhotoAndLogToSd(fb, stuId, stuName, subjCode, attStat, ts);

          for (int i = 0; i < 2; i++) {
            digitalWrite(ONBOARD_LED_PIN, LOW);
            delay(80);
            digitalWrite(ONBOARD_LED_PIN, HIGH);
            delay(80);
          }
        }
        delay(1200);
      }
    }
  } else {
    bufferFrameSilentlyWhenOffline(fb, ts);
  }

  http.end();
  esp_camera_fb_return(fb);
}