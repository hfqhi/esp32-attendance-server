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
bool serverLinked   = false;
bool setupApRunning = false;

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
const unsigned long CAPTURE_INTERVAL_MS = 1000;
const unsigned long HEARTBEAT_INTERVAL_MS = 6000;

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
void ensureFlashLedOff() {
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
}

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

void startSetupApIfNeeded() {
  if (setupApRunning) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ES-SEE-CAM-Setup");
  setupApRunning = true;
  Serial.println("[SETUP AP] Active: Connect to 'ES-SEE-CAM-Setup' and open http://192.168.4.1");
}

void stopSetupApIfLinked() {
  if (!setupApRunning) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  setupApRunning = false;
  Serial.println("[SETUP AP] Server verified — closed setup hotspot.");
}

void handleScanWifi() {
  camHttpServer.sendHeader("Access-Control-Allow-Origin", "*");
  int n = WiFi.scanNetworks(false, false);
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = s;
    obj["rssi"] = WiFi.RSSI(i);
    obj["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  camHttpServer.send(200, "application/json", out);
}

void handlePortalPage() {
  String staStatus = (WiFi.status() == WL_CONNECTED)
    ? ("Connected to <b>" + WiFi.SSID() + "</b> (" + WiFi.localIP().toString() + ")")
    : "Not connected to router yet";
  String srvStatus = serverLinked
    ? ("Linked to Server (" + serverBase + ")")
    : ("Waiting for Server at " + serverBase);

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ES-SEE Camera Wi-Fi Setup</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #080c14; color: #f1f5f9; display: flex; justify-content: center; padding: 18px; margin: 0; }
    .card { background: #0d1424; border: 1px solid #1e293b; border-radius: 14px; padding: 20px; width: 100%; max-width: 400px; }
    h2 { margin: 0 0 6px; font-size: 18px; color: #38bdf8; }
    .status { background: #0f172a; border: 1px solid #1e293b; border-radius: 8px; padding: 10px; font-size: 12px; color: #cbd5e1; margin-bottom: 14px; line-height: 1.5; }
    label { display: block; font-size: 12px; margin-bottom: 4px; color: #cbd5e1; }
    input, select { width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 12px; border-radius: 8px; border: 1px solid #334155; background: #0f172a; color: #fff; font-size: 14px; }
    .row { display: flex; gap: 8px; margin-bottom: 12px; }
    .btn-sec { padding: 10px 12px; border: 1px solid #334155; border-radius: 8px; background: #1e293b; color: #38bdf8; font-weight: 600; font-size: 12px; cursor: pointer; white-space: nowrap; }
    .btn-pri { width: 100%; padding: 12px; border: none; border-radius: 8px; background: #0284c7; color: #fff; font-weight: 700; font-size: 14px; cursor: pointer; }
    .hint { font-size: 11px; color: #94a3b8; margin-top: 10px; line-height: 1.4; }
  </style>
</head>
<body>
  <div class="card">
    <h2>ES-SEE Camera Wi-Fi Setup</h2>
    <div class="status">
      <div>Wi-Fi: )rawliteral" + staStatus + R"rawliteral(</div>
      <div>Server: )rawliteral" + srvStatus + R"rawliteral(</div>
    </div>
    <form action="/save_wifi" method="POST">
      <label>2.4 GHz Wi-Fi Network (SSID)</label>
      <div class="row">
        <select id="ssidSelect" onchange="document.getElementById('ssidInput').value=this.value" style="margin-bottom:0">
          <option value="">-- Tap Scan 2.4G Wi-Fi --</option>
        </select>
        <button type="button" class="btn-sec" onclick="scanNetworks()">Scan</button>
      </div>
      <input id="ssidInput" type="text" name="ssid" value=")rawliteral" + wifiSsid + R"rawliteral(" placeholder="Or type exact 2.4 GHz SSID" required>
      <label>Wi-Fi Password</label>
      <input type="password" name="pass" placeholder="Enter Wi-Fi password">
      <label>Laptop Server URL (Check Dashboard Top Bar)</label>
      <input type="text" name="server" value=")rawliteral" + serverBase + R"rawliteral(" placeholder="http://192.168.1.x:5000" required>
      <button type="submit" class="btn-pri">Save & Connect</button>
    </form>
    <p class="hint">Note: ESP32-CAM only supports <b>2.4 GHz</b> Wi-Fi. Make sure your laptop and ESP32-CAM are on the same router/hotspot.</p>
  </div>
  <script>
    async function scanNetworks() {
      const sel = document.getElementById('ssidSelect');
      sel.innerHTML = '<option value="">Scanning...</option>';
      try {
        const res = await fetch('/scan_wifi');
        const data = await res.json();
        sel.innerHTML = '<option value="">-- Select Network --</option>';
        (data.networks || []).forEach(n => {
          const opt = document.createElement('option');
          opt.value = n.ssid;
          opt.textContent = `${n.ssid} (${n.rssi} dBm)`;
          sel.appendChild(opt);
        });
      } catch (e) {
        sel.innerHTML = '<option value="">Scan failed — type SSID below</option>';
      }
    }
    scanNetworks();
  </script>
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
  camHttpServer.send(200, "text/html", "<body style='background:#080c14;color:#34d399;font-family:sans-serif;text-align:center;padding:40px'><h3>Saved! Connecting ESP32-CAM...</h3><p style='color:#94a3b8;font-size:13px'>If it fails to connect, ES-SEE-CAM-Setup will remain open at 192.168.4.1</p></body>");
  delay(1000);
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
  camHttpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Saved! ESP32-CAM is reconnecting...\"}");
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
// 5. CAMERA (NO FB-OVF + CAMERAWEBSERVER BRIGHTNESS) & SD DATABASE
// ==========================================
camera_fb_t* grabFreshFrame() {
  camera_fb_t* stale = esp_camera_fb_get();
  if (stale) {
    esp_camera_fb_return(stale);
  }
  return esp_camera_fb_get();
}

bool initCamera() {
  ensureFlashLedOff();

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(120);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(120);

  camera_config_t config = {};
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.sccb_i2c_port = 0;

  config.xclk_freq_hz  = 16000000;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK && psramFound()) {
    Serial.printf("[CAM] PSRAM init returned 0x%x, retrying in DRAM...\n", err);
    config.frame_size   = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    err = esp_camera_init(&config);
  }

  if (err != ESP_OK) {
    Serial.printf("[CAM] Init error 0x%x\n", err);
    return false;
  }

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

  for (int i = 0; i < 15; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(45);
  }

  return true;
}

bool initMicroSD() {
  // 1-bit SD mode frees GPIO 4, then immediately drive GPIO 4 LOW to extinguish Flash LED
  if (!SD_MMC.begin("/sdcard", true)) {
    ensureFlashLedOff();
    Serial.println("[SD] Mount failed or no SD card inserted.");
    return false;
  }
  ensureFlashLedOff();

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
bool sendHeartbeatToServer() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(serverBase + "/api/esp32/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2500);

  String body = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"ssid\":\"" + WiFi.SSID() + "\",\"server_url\":\"" + serverBase + "\"}";
  int code = http.POST(body);
  http.end();

  if (code == 200) {
    if (!serverLinked) {
      Serial.printf("[SYNC] Linked to server at %s\n", serverBase.c_str());
    }
    serverLinked = true;
    stopSetupApIfLinked();
    return true;
  } else {
    Serial.printf("[SYNC] Server unreachable (%s) -> HTTP %d\n", serverBase.c_str(), code);
    serverLinked = false;
    startSetupApIfNeeded();
    return false;
  }
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
  camera_fb_t* fb = grabFreshFrame();
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

  ensureFlashLedOff();
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  loadNetworkConfig();

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed.");
    while (true) delay(1000);
  }

  sdAvailable = initMicroSD();
  ensureFlashLedOff();

  // Keep setup AP active alongside STA until server registration succeeds
  startSetupApIfNeeded();

  if (wifiSsid.length() > 0) {
    WiFi.setSleep(false);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    Serial.printf("[WIFI] Connecting to '%s'", wifiSsid.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 24) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();
  }

  camHttpServer.on("/", HTTP_GET, handlePortalPage);
  camHttpServer.on("/scan_wifi", HTTP_GET, handleScanWifi);
  camHttpServer.on("/save_wifi", HTTP_POST, handlePortalSave);
  camHttpServer.on("/wifi_config", HTTP_POST, handleApiWifiConfig);
  camHttpServer.on("/wifi_config", HTTP_OPTIONS, handleApiWifiConfig);
  camHttpServer.on("/capture", HTTP_GET, handleSingleCapture);
  camHttpServer.begin();
  streamServer.begin();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Router IP: http://%s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, DAYLIGHT_OFF, NTP_SERVER);

    if (sendHeartbeatToServer()) {
      syncEnrolledPortraitsFromServer();
      syncOfflineBufferToServer();
    }
  } else {
    Serial.println("[WIFI] Router not connected yet. Open http://192.168.4.1 on 'ES-SEE-CAM-Setup'.");
  }
}

void loop() {
  camHttpServer.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    serviceMjpegStream();

    if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = millis();
      sendHeartbeatToServer();
    }
  } else {
    startSetupApIfNeeded();
  }

  if (millis() - lastCaptureMs < CAPTURE_INTERVAL_MS) return;
  lastCaptureMs = millis();

  if (!serverLinked && !sdAvailable) return;

  camera_fb_t* fb = grabFreshFrame();
  if (!fb) return;

  if (!hasSignificantMotion(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  TimeStamps ts;
  getCurrentTimestamps(ts);

  if (WiFi.status() != WL_CONNECTED || !serverLinked) {
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