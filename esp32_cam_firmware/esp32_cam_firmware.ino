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
bool serverReachable = false;
unsigned long lastSyncMs = 0;
unsigned long lastOfflineCheckMs = 0;
unsigned long lastOfflineSaveMs = 0;

const unsigned long SYNC_INTERVAL_MS = 3500;
const unsigned long OFFLINE_CHECK_INTERVAL_MS = 1500;
const unsigned long OFFLINE_SAVE_COOLDOWN_MS = 6000;
const int MAX_OFFLINE_BUFFER_FILES = 25;

uint8_t prevSample[64] = {0};
size_t prevFrameLen = 0;
RTC_DATA_ATTR uint32_t bufferCounter = 1;

// ==========================================
// 2. TIMESTAMP & SD DIRECTORY HELPERS
// ==========================================
struct TimeStamps {
  String dayName;    // Friday
  String dateIso;    // 2026-11-20
  String timeFile;   // 07-10-00PM
  String timeHuman;  // 07:10:00 PM
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

void ensureParentDirs(const String& filePath) {
  int slashIdx = 1;
  while ((slashIdx = filePath.indexOf('/', slashIdx)) != -1) {
    String subDir = filePath.substring(0, slashIdx);
    if (subDir.length() > 0 && !SD_MMC.exists(subDir)) {
      SD_MMC.mkdir(subDir);
    }
    slashIdx++;
  }
}

void ensureDir(const String& path) {
  if (!SD_MMC.exists(path)) {
    SD_MMC.mkdir(path);
  }
}

String urlEncodeParam(const String& str) {
  String encoded = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

// ==========================================
// 3. STRICT OFFLINE MOTION GATE (NO RANDOM SPAM)
// ==========================================
bool hasSignificantOfflineMotion(camera_fb_t* fb) {
  if (!fb || fb->len < 2048) return false;

  long lenDiff = abs((long)fb->len - (long)prevFrameLen);
  bool sizeChanged = (prevFrameLen > 0) && (lenDiff > (long)(prevFrameLen * 0.16));

  uint8_t currSample[64];
  size_t usable = fb->len > 512 ? fb->len - 512 : fb->len;
  size_t step = usable / 64;
  if (step == 0) step = 1;

  int diffCount = 0;
  for (int i = 0; i < 64; i++) {
    size_t idx = 256 + i * step;
    if (idx >= fb->len) idx = fb->len - 1;
    currSample[i] = fb->buf[idx];
    if (abs((int)currSample[i] - (int)prevSample[i]) > 28) {
      diffCount++;
    }
    prevSample[i] = currSample[i];
  }
  bool firstRun = (prevFrameLen == 0);
  prevFrameLen = fb->len;

  if (firstRun) return false;
  return sizeChanged && (diffCount >= 26);
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
  if (!SD_MMC.begin("/sdcard", true)) {
    keepFlashGroundedOff();
    Serial.println("[SD] Mount failed. Verify FAT32 format.");
    return false;
  }
  keepFlashGroundedOff();

  if (SD_MMC.cardType() == CARD_NONE) return false;

  ensureDir("/enrolled_faces");
  ensureDir("/offline_buffer");
  Serial.println("[SD] Ready (Two-way sync enabled).");
  return true;
}

// ==========================================
// 5. OFFLINE BUFFERING & AUTOMATIC CLEANUP
// ==========================================
void clearAllOfflineBufferFiles() {
  if (!sdAvailable) return;
  File root = SD_MMC.open("/offline_buffer");
  if (!root || !root.isDirectory()) return;

  File f = root.openNextFile();
  while (f) {
    String p = String(f.path());
    f.close();
    SD_MMC.remove(p);
    f = root.openNextFile();
  }
  root.close();
  keepFlashGroundedOff();
}

void bufferFrameOnlyWhenOffline(camera_fb_t* fb, const TimeStamps& ts) {
  if (!sdAvailable || !fb) return;
  if (millis() - lastOfflineSaveMs < OFFLINE_SAVE_COOLDOWN_MS) return;

  File root = SD_MMC.open("/offline_buffer");
  int count = 0;
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) { count++; f.close(); f = root.openNextFile(); }
    root.close();
  }
  if (count >= MAX_OFFLINE_BUFFER_FILES) return;

  String fullPath = "/offline_buffer/BUF_" + ts.dateIso + "_" + ts.timeFile + ".jpg";
  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.close();
    lastOfflineSaveMs = millis();
    Serial.printf("[OFFLINE-BUF] Buffered frame: %s\n", fullPath.c_str());
  }
  keepFlashGroundedOff();
}

void flushAndDeleteOfflineBufferToServer() {
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
        http.end();
        free(buf);

        // Automatically delete processed offline buffer file once server responds
        if (code == 200) {
          SD_MMC.remove(entryPath);
          Serial.printf("[OFFLINE-BUF] Synced & auto-deleted: %s\n", entryPath.c_str());
        }
      } else {
        entry.close();
        SD_MMC.remove(entryPath);
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
// 6. TWO-WAY LOCAL DB <-> MICROSD SYNCHRONIZATION
// ==========================================
void acknowledgeSyncTask(int taskId) {
  HTTPClient ack;
  ack.begin(String(SERVER_BASE) + "/api/esp32/sync_ack");
  ack.addHeader("Content-Type", "application/json");
  ack.setTimeout(2000);
  ack.POST("{\"id\":" + String(taskId) + "}");
  ack.end();
}

bool downloadFileFromServerToSd(const String& localRelPath, const String& sdPath) {
  if (!sdAvailable) return false;
  ensureParentDirs(sdPath);

  HTTPClient dl;
  String url = String(SERVER_BASE) + "/api/esp32/download_sync_file?path=" + urlEncodeParam(localRelPath);
  dl.begin(url);
  dl.setTimeout(4000);

  int code = dl.GET();
  if (code == 200) {
    if (SD_MMC.exists(sdPath)) {
      SD_MMC.remove(sdPath);
    }
    File out = SD_MMC.open(sdPath, FILE_WRITE);
    if (out) {
      dl.writeToStream(&out);
      out.close();
      dl.end();
      keepFlashGroundedOff();
      Serial.printf("[SD-SYNC] Wrote %s\n", sdPath.c_str());
      return true;
    }
  } else if (code == 404) {
    dl.end();
    return true;
  }
  dl.end();
  keepFlashGroundedOff();
  return false;
}

void processPendingSdSyncTasks() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/sync_tasks");
  http.setTimeout(2500);

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray tasks = doc["tasks"].as<JsonArray>();
      for (JsonObject t : tasks) {
        int taskId          = t["id"] | -1;
        String action       = t["action"] | "";
        String sdPath       = t["sd_path"] | "";
        String localRelPath = t["local_rel_path"] | "";

        if (taskId < 0 || sdPath.length() == 0) continue;

        bool ok = false;
        if (action == "DOWNLOAD_FILE") {
          ok = downloadFileFromServerToSd(localRelPath, sdPath);
        } else if (action == "DELETE_FILE") {
          if (SD_MMC.exists(sdPath)) {
            SD_MMC.remove(sdPath);
            Serial.printf("[SD-SYNC] Deleted %s\n", sdPath.c_str());
          }
          ok = true;
        }

        if (ok) {
          acknowledgeSyncTask(taskId);
        }
      }
    }
  }
  http.end();
  keepFlashGroundedOff();
}

void syncInitialEnrolledPortraits() {
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
          downloadFileFromServerToSd("enrolled_faces/" + fname, sdPath);
        }
      }
    }
  }
  http.end();
  keepFlashGroundedOff();
}

bool sendHeartbeatToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverReachable = false;
    return false;
  }

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);

  String body = "{\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  int code = http.POST(body);
  http.end();

  bool wasOffline = !serverReachable;
  serverReachable = (code == 200);

  if (serverReachable && wasOffline) {
    flushAndDeleteOfflineBufferToServer();
    syncInitialEnrolledPortraits();
  }
  return serverReachable;
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
  Serial.println("\n=== CPE-3B ESP32-CAM v2 Two-Way Sync Node ===");

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

    if (sendHeartbeatToServer()) {
      flushAndDeleteOfflineBufferToServer();
      syncInitialEnrolledPortraits();
      processPendingSdSyncTasks();
    }
  } else {
    Serial.println("\n[WIFI] Offline — standing by");
  }
  keepFlashGroundedOff();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    camHttpServer.handleClient();
    serviceMjpegStream();
    yield();

    if (millis() - lastSyncMs >= SYNC_INTERVAL_MS) {
      lastSyncMs = millis();
      if (sendHeartbeatToServer()) {
        processPendingSdSyncTasks();
      }
    }
  } else {
    serverReachable = false;
  }

  // Only buffer frames to /offline_buffer when Wi-Fi or the Flask server is unreachable
  if (!serverReachable && (millis() - lastOfflineCheckMs >= OFFLINE_CHECK_INTERVAL_MS)) {
    lastOfflineCheckMs = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      if (hasSignificantOfflineMotion(fb)) {
        TimeStamps ts = getCurrentTimestamps();
        bufferFrameOnlyWhenOffline(fb, ts);
      }
      esp_camera_fb_return(fb);
    }
  }
}