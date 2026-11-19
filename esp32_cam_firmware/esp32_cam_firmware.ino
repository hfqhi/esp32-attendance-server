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
const char* WIFI_SSID     = "PLDTHOMEFIBR356A1";
const char* WIFI_PASSWORD = "PLDTWIFI77GEC";

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
#define ONBOARD_LED_PIN   33

WebServer camHttpServer(80);
WiFiServer streamServer(81);
WiFiClient streamClient;

bool sdAvailable = false;
unsigned long lastCaptureMs = 0;
unsigned long lastHeartbeatMs = 0;
const unsigned long CAPTURE_INTERVAL_MS = 1300;
const unsigned long HEARTBEAT_INTERVAL_MS = 8000;

// Motion/Presence detection buffer (samples JPEG payload variance & byte signature)
uint8_t prevSample[64] = {0};
size_t prevFrameLen = 0;
RTC_DATA_ATTR uint32_t offlineCounter = 1;

// ==========================================
// 2. TIMESTAMP HELPERS
// ==========================================
struct TimeStamps {
  String dateFile;   // 11-18-2026
  String dateIso;    // 2026-11-18
  String timeFile;   // 09-15-30PM
  String timeHuman;  // 09:15:30 PM
};

TimeStamps getCurrentTimestamps() {
  TimeStamps ts;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    char dFile[16], dIso[16], tFile[16], tHuman[16];
    strftime(dFile, sizeof(dFile), "%m-%d-%Y", &timeinfo);
    strftime(dIso, sizeof(dIso), "%Y-%m-%d", &timeinfo);
    strftime(tFile, sizeof(tFile), "%I-%M-%S%p", &timeinfo);
    strftime(tHuman, sizeof(tHuman), "%I:%M:%S %p", &timeinfo);
    ts.dateFile = String(dFile);
    ts.dateIso = String(dIso);
    ts.timeFile = String(tFile);
    ts.timeHuman = String(tHuman);
  } else {
    // Fallback if booted completely offline with no NTP sync yet
    char idxBuf[16];
    snprintf(idxBuf, sizeof(idxBuf), "SEQ-%04lu", (unsigned long)offlineCounter++);
    ts.dateFile = "Offline_Unsynced";
    ts.dateIso = "";
    ts.timeFile = String(idxBuf);
    ts.timeHuman = String(idxBuf);
  }
  return ts;
}

String sanitizeName(const String& raw) {
  String clean = "";
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (isalnum(c) || c == '-' || c == '_') clean += c;
    else if (c == ' ') clean += '_';
  }
  return clean.length() > 0 ? clean : "Student";
}

// ==========================================
// 3. PRESENCE / MOTION GATE (PREVENTS RANDOM EMPTY PICS)
// ==========================================
bool hasSceneChanged(camera_fb_t* fb) {
  if (!fb || fb->len < 1024) return false;

  // Check JPEG size delta (> 6% change indicates someone stepped in/out of frame)
  long lenDiff = abs((long)fb->len - (long)prevFrameLen);
  bool sizeChanged = (prevFrameLen == 0) || (lenDiff > (long)(prevFrameLen * 0.06));

  // Also sample 64 evenly spaced payload bytes to detect subtle movement
  uint8_t currSample[64];
  size_t step = (fb->len - 512) / 64;
  int diffCount = 0;
  for (int i = 0; i < 64; i++) {
    currSample[i] = fb->buf[256 + i * step];
    if (abs((int)currSample[i] - (int)prevSample[i]) > 18) {
      diffCount++;
    }
    prevSample[i] = currSample[i];
  }
  prevFrameLen = fb->len;

  return sizeChanged || (diffCount >= 18);
}

// ==========================================
// 4. CAMERA & MICROSD SETUP
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

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count     = 1;
  }

  return esp_camera_init(&config) == ESP_OK;
}

bool initMicroSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] Mount failed. Verify FAT32 format.");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) return false;

  const char* dirs[] = {"/enrolled_faces", "/captured_faces", "/offline_queue", "/logs"};
  for (const char* d : dirs) {
    if (!SD_MMC.exists(d)) SD_MMC.mkdir(d);
  }
  Serial.println("[SD] Systematic folders ready (/enrolled_faces, /captured_faces, /offline_queue, /logs).");
  return true;
}

void logAttendanceToSd(const String& studentName, const String& subjectCode, const String& status, const TimeStamps& ts) {
  if (!sdAvailable) return;
  String csvPath = "/logs/attendance_" + ts.dateFile + ".csv";
  bool isNew = !SD_MMC.exists(csvPath);

  File f = SD_MMC.open(csvPath, isNew ? FILE_WRITE : FILE_APPEND);
  if (f) {
    if (isNew) {
      f.println("Student Name,Subject,Status,Date,Time");
    }
    f.printf("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
             studentName.c_str(), subjectCode.c_str(), status.c_str(),
             ts.dateFile.c_str(), ts.timeHuman.c_str());
    f.close();
  }
}

void saveVerifiedCapture(camera_fb_t* fb, const String& studentName, const TimeStamps& ts) {
  if (!sdAvailable || !fb) return;
  String dateDir = "/captured_faces/" + ts.dateFile;
  if (!SD_MMC.exists(dateDir)) SD_MMC.mkdir(dateDir);

  String fileName = sanitizeName(studentName) + "_" + ts.dateFile + "_" + ts.timeFile + ".jpg";
  String fullPath = dateDir + "/" + fileName;

  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
    Serial.printf("[SD] Saved verified capture: %s\n", fullPath.c_str());
  }
}

void saveOfflineQueueCapture(camera_fb_t* fb, const TimeStamps& ts) {
  if (!sdAvailable || !fb) return;
  String fileName = "Offline_" + ts.dateFile + "_" + ts.timeFile + ".jpg";
  String fullPath = "/offline_queue/" + fileName;

  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
    Serial.printf("[SD-OFFLINE] Queued face capture: %s\n", fullPath.c_str());
    logAttendanceToSd("Offline_Queued", "OFFLINE", "PENDING_SYNC", ts);
  }
}

// ==========================================
// 5. SERVER SYNC: REGISTER IP, ENROLLED PORTRAITS & OFFLINE QUEUE
// ==========================================
void sendHeartbeatToServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2500);

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
              Serial.printf("[SD] Synced enrolled portrait: %s\n", sdPath.c_str());
            }
          }
          dl.end();
        }
      }
    }
  }
  http.end();
}

void syncOfflineQueueToServer() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;
  File root = SD_MMC.open("/offline_queue");
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
        http.setTimeout(4000);

        int code = http.POST(buf, fsize);
        if (code == 200) {
          JsonDocument doc;
          if (!deserializeJson(doc, http.getString())) {
            const char* status = doc["status"] | "UNKNOWN";
            const char* stuName = doc["student_name"] | "Unknown";
            const char* subjCode = doc["subject_code"] | "SYNC";
            TimeStamps ts = getCurrentTimestamps();

            if (strcmp(status, "LOGGED") == 0) {
              String destDir = "/captured_faces/" + ts.dateFile;
              if (!SD_MMC.exists(destDir)) SD_MMC.mkdir(destDir);
              String destPath = destDir + "/" + sanitizeName(stuName) + "_" + ts.dateFile + "_" + ts.timeFile + ".jpg";
              File dest = SD_MMC.open(destPath, FILE_WRITE);
              if (dest) {
                dest.write(buf, fsize);
                dest.close();
              }
              logAttendanceToSd(stuName, subjCode, "PRESENT", ts);
              Serial.printf("[SYNC] Logged offline capture for %s\n", stuName);
            }
            // Remove processed file from offline_queue
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
}

// ==========================================
// 6. WEB DASHBOARD CAMERA STREAMING ENDPOINTS
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
// 7. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== CPE-3B Smart ESP32-CAM Node ===");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed.");
    while (true) delay(1000);
  }

  sdAvailable = initMicroSD();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
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
    syncOfflineQueueToServer();
  } else {
    Serial.println("\n[WIFI] Offline mode — capturing motion faces to /offline_queue/");
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    camHttpServer.handleClient();
    serviceMjpegStream();

    if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = millis();
      sendHeartbeatToServer();
    }
  }

  if (millis() - lastCaptureMs < CAPTURE_INTERVAL_MS) return;
  lastCaptureMs = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  // Gate 1: Skip empty/static frames — zero random photos or spam logs
  if (!hasSceneChanged(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  TimeStamps ts = getCurrentTimestamps();

  if (WiFi.status() != WL_CONNECTED) {
    saveOfflineQueueCapture(fb, ts);
    esp_camera_fb_return(fb);
    return;
  }

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/verify_frame");
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(3500);

  int httpCode = http.POST(fb->buf, fb->len);
  if (httpCode == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      const char* status   = doc["status"]  | "UNKNOWN";
      const char* message  = doc["message"] | "";
      const char* stuName  = doc["student_name"] | "Student";
      const char* subjCode = doc["subject_code"] | "";
      const char* attStat  = doc["attendance_status"] | "PRESENT";

      Serial.printf("[%s] %s\n", status, message);

      // Only save JPEG & CSV log when an enrolled student is verified & logged
      if (strcmp(status, "LOGGED") == 0) {
        saveVerifiedCapture(fb, stuName, ts);
        logAttendanceToSd(stuName, subjCode, attStat, ts);

        for (int i = 0; i < 2; i++) {
          digitalWrite(ONBOARD_LED_PIN, LOW);
          delay(120);
          digitalWrite(ONBOARD_LED_PIN, HIGH);
          delay(120);
        }
        delay(2000);
      }
    }
  } else {
    // Server unreachable even though Wi-Fi is up -> queue to SD card
    saveOfflineQueueCapture(fb, ts);
  }

  http.end();
  esp_camera_fb_return(fb);
}