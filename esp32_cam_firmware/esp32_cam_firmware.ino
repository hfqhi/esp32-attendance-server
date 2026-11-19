#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"

// Forward declaration for Arduino IDE auto-generated function prototypes
struct TimeStamps;

// ==========================================
// 1. PERSISTENT WI-FI & SERVER CONFIGURATION (NVS FLASH)
// ==========================================
Preferences prefs;
String wifiSsid     = "";
String wifiPassword = "";
String serverBase   = "http://192.168.1.100:5000";
bool apFallbackMode = false;

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
const unsigned long CAPTURE_INTERVAL_MS = 1000;
const unsigned long HEARTBEAT_INTERVAL_MS = 8000;

uint8_t prevSample[64] = {0};
size_t prevFrameLen = 0;
RTC_DATA_ATTR uint32_t bufferCounter = 1;

struct TimeStamps {
  String dayName;
  String dateIso;
  String timeFile;
  String timeHuman;
  bool validNtp;
};

// ==========================================
// 2. NVS STORAGE & CAPTIVE PORTAL HELPERS
// ==========================================
void loadNetworkConfig() {
  prefs.begin("essee_cfg", true);
  wifiSsid     = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");
  serverBase   = prefs.getString("server", "http://192.168.1.100:5000");
  prefs.end();
}

void saveNetworkConfig(const String& newSsid, const String& newPass, const String& newServer) {
  prefs.begin("essee_cfg", false);
  if (newSsid.length() > 0) prefs.putString("ssid", newSsid);
  prefs.putString("pass", newPass);
  if (newServer.length() > 0) {
    String cleanUrl = newServer;
    cleanUrl.trim();
    if (!cleanUrl.startsWith("http://") && !cleanUrl.startsWith("https://")) {
      cleanUrl = "http://" + cleanUrl;
    }
    if (cleanUrl.endsWith("/")) {
      cleanUrl.remove(cleanUrl.length() - 1);
    }
    prefs.putString("server", cleanUrl);
  }
  prefs.end();
}

void handlePortalPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ES-SEE Camera Wi-Fi Setup</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #080c14; color: #f1f5f9; display: flex; justify-content: center; padding: 24px; }
    .card { background: #0d1424; border: 1px solid #1e293b; border-radius: 14px; padding: 22px; width: 100%; max-width: 380px; }
    h2 { margin: 0 0 6px; font-size: 18px; color: #38bdf8; }
    p { margin: 0 0 16px; font-size: 12px; color: #94a3b8; }
    label { display: block; font-size: 12px; margin-bottom: 4px; color: #cbd5e1; }
    input { width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 12px; border-radius: 8px; border: 1px solid #334155; background: #0f172a; color: #fff; font-size: 14px; }
    button { width: 100%; padding: 11px; border: none; border-radius: 8px; background: #0284c7; color: #fff; font-weight: 700; font-size: 14px; cursor: pointer; }
  </style>
</head>
<body>
  <div class="card">
    <h2>ES-SEE Camera Wi-Fi Setup</h2>
    <p>Configure classroom Wi-Fi and Attendance Server IP.</p>
    <form action="/save_wifi" method="POST">
      <label>Wi-Fi Network Name (SSID)</label>
      <input type="text" name="ssid" value=")rawliteral" + wifiSsid + R"rawliteral(" required>
      <label>Wi-Fi Password</label>
      <input type="password" name="pass" placeholder="Enter Wi-Fi password">
      <label>Server URL (Laptop IP)</label>
      <input type="text" name="server" value=")rawliteral" + serverBase + R"rawliteral(" required>
      <button type="submit">Save & Connect</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
  camHttpServer.send(200, "text/html", html);
}

void handlePortalSave() {
  String newSsid = camHttpServer.arg("ssid");
  String newPass = camHttpServer.arg("pass");
  String newServer = camHttpServer.arg("server");
  saveNetworkConfig(newSsid, newPass, newServer);
  camHttpServer.send(200, "text/html", "<body style='background:#080c14;color:#34d399;font-family:sans-serif;text-align:center;padding:40px'><h3>Saved! Restarting ESP32-CAM...</h3></body>");
  delay(1200);
  ESP.restart();
}

void handleApiWifiConfig() {
  camHttpServer.sendHeader("Access-Control-Allow-Origin", "*");
  if (camHttpServer.method() == HTTP_OPTIONS) {
    camHttpServer.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    camHttpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    camHttpServer.send(204);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, camHttpServer.arg("plain"))) {
    camHttpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  String newSsid   = doc["ssid"] | "";
  String newPass   = doc["password"] | "";
  String newServer = doc["server_url"] | serverBase;

  if (newSsid.length() == 0) {
    camHttpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Wi-Fi SSID is required\"}");
    return;
  }

  saveNetworkConfig(newSsid, newPass, newServer);
  camHttpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Wi-Fi credentials saved. Reconnecting ESP32-CAM...\"}");
  delay(800);
  ESP.restart();
}

// ==========================================
// 3. TIMESTAMP & PATH HELPERS
// ==========================================
void getCurrentTimestamps(TimeStamps& ts) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
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

// ==========================================
// 4. PRESENCE / MOTION GATE
// ==========================================
bool hasSignificantMotion(camera_fb_t* fb) {
  if (!fb || fb->len < 1024) return false;

  long lenDiff = abs((long)fb->len - (long)prevFrameLen);
  bool sizeChanged = (prevFrameLen == 0) || (lenDiff > (long)(prevFrameLen * 0.10));

  uint8_t currSample[64];
  size_t usable = fb->len > 512 ? fb->len - 512 : fb->len;
  size_t step = usable / 64;
  if (step == 0) step = 1;

  int diffCount = 0;
  for (int i = 0; i < 64; i++) {
    size_t idx = 256 + i * step;
    if (idx >= fb->len) idx = fb->len - 1;
    currSample[i] = fb->buf[idx];
    if (abs((int)currSample[i] - (int)prevSample[i]) > 22) {
      diffCount++;
    }
    prevSample[i] = currSample[i];
  }
  prevFrameLen = fb->len;

  return sizeChanged || (diffCount >= 22);
}

// ==========================================
// 5. CAMERA (HIGH-FPS VGA + CAMERAWEBSERVER BRIGHTNESS) & SD DATABASE
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
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    return false;
  }

  // Match Espressif CameraWebServer exposure (aec2=0 prevents indoor spot-dimming)
  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, 1);
    s->set_aec_value(s, 300);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
  }

  // Let Auto-Exposure and Auto-White-Balance converge cleanly
  for (int i = 0; i < 15; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(45);
  }

  return true;
}

bool initMicroSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] Mount failed. Verify FAT32 format.");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) return false;

  ensureDir("/enrolled_faces");
  ensureDir("/attendance_captures");
  ensureDir("/logs");
  ensureDir("/offline_buffer");
  Serial.println("[SD] Photo & Log Database ready.");
  return true;
}

void logStructuredAttendanceToSd(const String& studentName, const String& subjectCode, const String& status, const TimeStamps& ts, const String& photoRelPath) {
  if (!sdAvailable) return;

  String daySlug = sanitizeSlug(ts.dayName, "Day");
  String dateSlug = sanitizeSlug(ts.dateIso, "Date");
  String subjSlug = sanitizeSlug(subjectCode, "General");

  String dayDir = "/logs/" + daySlug;
  String dateDir = dayDir + "/" + dateSlug;
  ensureDir(dayDir);
  ensureDir(dateDir);

  String csvPath = dateDir + "/" + subjSlug + "_attendance.csv";
  bool isNew = !SD_MMC.exists(csvPath);

  File f = SD_MMC.open(csvPath, isNew ? FILE_WRITE : FILE_APPEND);
  if (f) {
    if (isNew) {
      f.println("Student Name,Subject Code,Status,Day,Date,Time,SD Photo Path");
    }
    f.printf("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
             studentName.c_str(), subjSlug.c_str(), status.c_str(),
             ts.dayName.c_str(), ts.dateIso.c_str(), ts.timeHuman.c_str(),
             photoRelPath.c_str());
    f.close();
  }
}

String saveStructuredAttendanceCapture(camera_fb_t* fb, const String& studentName, const String& subjectCode, const TimeStamps& ts) {
  if (!sdAvailable || !fb) return "";

  String daySlug = sanitizeSlug(ts.dayName, "Day");
  String dateSlug = sanitizeSlug(ts.dateIso, "Date");
  String subjSlug = sanitizeSlug(subjectCode, "General");
  String stuSlug = sanitizeSlug(studentName, "Student");

  String dayDir = "/attendance_captures/" + daySlug;
  String dateDir = dayDir + "/" + dateSlug;
  String subjDir = dateDir + "/" + subjSlug;
  ensureDir(dayDir);
  ensureDir(dateDir);
  ensureDir(subjDir);

  String fileName = stuSlug + "_" + subjSlug + "_" + dateSlug + "_" + ts.timeFile + ".jpg";
  String fullPath = subjDir + "/" + fileName;

  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
    return fullPath;
  }
  return "";
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
}

// ==========================================
// 6. SERVER SYNC: REGISTER IP, PORTRAITS & OFFLINE BUFFER
// ==========================================
void sendHeartbeatToServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(serverBase + "/api/esp32/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2500);

  String body = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"ssid\":\"" + WiFi.SSID() + "\",\"server_url\":\"" + serverBase + "\"}";
  http.POST(body);
  http.end();
}

void syncEnrolledPortraitsFromServer() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(serverBase + "/api/esp32/enrolled_portraits");
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
          dl.begin(serverBase + "/api/esp32/portraits/" + fname);
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
        http.begin(serverBase + "/api/esp32/sync_offline");
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("X-Camera-Source", "esp32");
        http.setTimeout(3500);

        int code = http.POST(buf, fsize);
        if (code == 200) {
          JsonDocument doc;
          if (!deserializeJson(doc, http.getString())) {
            const char* status = doc["status"] | "UNKNOWN";
            const char* stuName = doc["student_name"] | "";
            const char* subjCode = doc["subject_code"] | "General";
            const char* attStat = doc["attendance_status"] | "PRESENT";
            TimeStamps ts;
            getCurrentTimestamps(ts);

            if (strcmp(status, "LOGGED") == 0 && strlen(stuName) > 0) {
              camera_fb_t fakeFb;
              fakeFb.buf = buf;
              fakeFb.len = fsize;
              String savedPath = saveStructuredAttendanceCapture(&fakeFb, stuName, subjCode, ts);
              logStructuredAttendanceToSd(stuName, subjCode, attStat, ts, savedPath);
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
}

// ==========================================
// 7. WEB DASHBOARD HIGH-FPS CAMERA STREAMING ENDPOINTS
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
      yield();
    }
  }
}

// ==========================================
// 8. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== ES-SEE Attendance Camera Node ===");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  loadNetworkConfig();

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed.");
    while (true) delay(1000);
  }

  sdAvailable = initMicroSD();

  bool connected = false;
  if (wifiSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    Serial.printf("[WIFI] Connecting to %s", wifiSsid.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 24) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  camHttpServer.on("/", HTTP_GET, handlePortalPage);
  camHttpServer.on("/save_wifi", HTTP_POST, handlePortalSave);
  camHttpServer.on("/wifi_config", HTTP_POST, handleApiWifiConfig);
  camHttpServer.on("/wifi_config", HTTP_OPTIONS, handleApiWifiConfig);
  camHttpServer.on("/capture", HTTP_GET, handleSingleCapture);

  if (connected) {
    Serial.printf("\n[WIFI] Online at http://%s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, DAYLIGHT_OFF, NTP_SERVER);

    camHttpServer.begin();
    streamServer.begin();

    sendHeartbeatToServer();
    syncEnrolledPortraitsFromServer();
    syncOfflineBufferToServer();
  } else {
    apFallbackMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ES-SEE-CAM-Setup");
    camHttpServer.begin();
    Serial.println("\n[SETUP AP] Connect phone/laptop to Wi-Fi 'ES-SEE-CAM-Setup' and open http://192.168.4.1");
  }
}

void loop() {
  camHttpServer.handleClient();

  if (apFallbackMode) {
    delay(10);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
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

  if (!hasSignificantMotion(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  TimeStamps ts;
  getCurrentTimestamps(ts);

  if (WiFi.status() != WL_CONNECTED) {
    bufferFrameSilentlyWhenOffline(fb, ts);
    esp_camera_fb_return(fb);
    return;
  }

  HTTPClient http;
  http.begin(serverBase + "/api/verify_frame");
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Camera-Source", "esp32");
  http.setTimeout(3000);

  int httpCode = http.POST(fb->buf, fb->len);
  if (httpCode == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      const char* status   = doc["status"]  | "UNKNOWN";
      const char* message  = doc["message"] | "";
      const char* stuName  = doc["student_name"] | "";
      const char* subjCode = doc["subject_code"] | "General";
      const char* attStat  = doc["attendance_status"] | "PRESENT";

      Serial.printf("[%s] %s\n", status, message);

      if ((strcmp(status, "LOGGED") == 0 || strcmp(status, "ALREADY_LOGGED") == 0) && strlen(stuName) > 0) {
        if (strcmp(status, "LOGGED") == 0) {
          String savedPath = saveStructuredAttendanceCapture(fb, stuName, subjCode, ts);
          logStructuredAttendanceToSd(stuName, subjCode, attStat, ts, savedPath);

          for (int i = 0; i < 2; i++) {
            digitalWrite(ONBOARD_LED_PIN, LOW);
            delay(100);
            digitalWrite(ONBOARD_LED_PIN, HIGH);
            delay(100);
          }
        }
        delay(1500);
      }
    }
  } else {
    bufferFrameSilentlyWhenOffline(fb, ts);
  }

  http.end();
  esp_camera_fb_return(fb);
}