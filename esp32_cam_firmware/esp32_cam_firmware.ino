#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
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

// ==========================================
// 2. HARDWARE PINS (AI-THINKER ESP32-CAM)
// ==========================================
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

// Conflict-Free Peripheral Pins (MB102 + I2C LCD + MM-FMD Buzzer)
#define LCD_SDA_PIN          13
#define LCD_SCL_PIN           3
#define BUZZER_PIN            1   // GPIO 1 (U0T)
#define BUZZER_ACTIVE_LEVEL  LOW  // MM-FMD Low-Level Trigger: LOW = Beep
#define BUZZER_IDLE_LEVEL    HIGH // MM-FMD Low-Level Trigger: HIGH = Silent

// PCF8574 I2C Backpack Bit Mapping (Standard HD44780 Backpack)
#define PCF_RS 0x01
#define PCF_RW 0x02
#define PCF_EN 0x04
#define PCF_BL 0x08

WebServer camHttpServer(80);
WiFiServer streamServer(81);
WiFiClient streamClient;

uint8_t lcdI2CAddr = 0;
bool lcdAvailable = false;
bool buzzerAvailable = false;
bool sdAvailable = false;
bool serverReachable = false;

unsigned long lastSyncMs = 0;
unsigned long lastReconcileMs = 0;
unsigned long lastLcdClockMs = 0;
unsigned long lastOfflineCheckMs = 0;
unsigned long lastOfflineSaveMs = 0;

const unsigned long SYNC_INTERVAL_MS = 1800;
const unsigned long RECONCILE_INTERVAL_MS = 15000;
const unsigned long LCD_CLOCK_INTERVAL_MS = 1000;
const unsigned long OFFLINE_CHECK_INTERVAL_MS = 1500;
const unsigned long OFFLINE_SAVE_COOLDOWN_MS = 6000;
const int MAX_OFFLINE_BUFFER_FILES = 25;

// LCD & Buzzer Live State
String currentClassStatus = "No Active Class";
String lcdLastRow1 = "";
String lcdLastRow2 = "";
bool lcdAlertActive = false;
unsigned long lcdAlertUntilMs = 0;
int lastHandledScanSeq = 0;

// Non-blocking Buzzer State Machine
int buzzerRemainingBeeps = 0;
unsigned long buzzerOnMs = 0;
unsigned long buzzerOffMs = 0;
unsigned long buzzerNextToggleMs = 0;
bool buzzerSounding = false;

// Offline Motion Gate State
uint8_t prevSample[64] = {0};
size_t prevFrameLen = 0;
RTC_DATA_ATTR uint32_t bufferCounter = 1;

// ==========================================
// 3. PIN-LOCKED DIRECT PCF8574 16x2 LCD DRIVER
// ==========================================
void pcfWriteExpander(uint8_t data) {
  if (lcdI2CAddr == 0) return;
  Wire.beginTransmission(lcdI2CAddr);
  Wire.write(data | PCF_BL);
  Wire.endTransmission();
}

void pcfPulseEnable(uint8_t data) {
  pcfWriteExpander(data | PCF_EN);
  delayMicroseconds(2);
  pcfWriteExpander(data & ~PCF_EN);
  delayMicroseconds(50);
}

void pcfWrite4Bits(uint8_t nibbleWithCtrl) {
  pcfWriteExpander(nibbleWithCtrl);
  pcfPulseEnable(nibbleWithCtrl);
}

void lcdSendByte(uint8_t value, uint8_t mode) {
  uint8_t highNib = (value & 0xF0) | mode;
  uint8_t lowNib  = ((value << 4) & 0xF0) | mode;
  pcfWrite4Bits(highNib);
  pcfWrite4Bits(lowNib);
}

void lcdCommand(uint8_t cmd) {
  lcdSendByte(cmd, 0x00);
}

void lcdWriteChar(uint8_t ch) {
  lcdSendByte(ch, PCF_RS);
}

void lcdSetCursor(uint8_t col, uint8_t row) {
  uint8_t rowOffset = (row == 0) ? 0x00 : 0x40;
  lcdCommand(0x80 | (col + rowOffset));
}

void lcdBeginHardware(uint8_t addr) {
  lcdI2CAddr = addr;
  delay(50);
  pcfWriteExpander(0x00);
  delay(20);

  // HD44780 4-bit initialization handshake
  pcfWrite4Bits(0x30);
  delay(5);
  pcfWrite4Bits(0x30);
  delayMicroseconds(200);
  pcfWrite4Bits(0x30);
  delayMicroseconds(200);
  pcfWrite4Bits(0x20);
  delayMicroseconds(200);

  lcdCommand(0x28); // 4-bit bus, 2 lines, 5x8 font
  lcdCommand(0x08); // Display OFF
  lcdCommand(0x01); // Clear display
  delay(3);
  lcdCommand(0x06); // Entry mode: increment cursor, no shift
  lcdCommand(0x0C); // Display ON, cursor OFF, blink OFF
}

String fitLcd16(const String& text) {
  String out = text;
  if (out.length() > 16) {
    out = out.substring(0, 16);
  }
  while (out.length() < 16) {
    out += ' ';
  }
  return out;
}

void writeLcdLines(const String& row1, const String& row2) {
  if (!lcdAvailable || lcdI2CAddr == 0) return;
  String r1 = fitLcd16(row1);
  String r2 = fitLcd16(row2);
  if (r1 != lcdLastRow1) {
    lcdSetCursor(0, 0);
    for (int i = 0; i < 16; i++) {
      lcdWriteChar((uint8_t)r1[i]);
    }
    lcdLastRow1 = r1;
  }
  if (r2 != lcdLastRow2) {
    lcdSetCursor(0, 1);
    for (int i = 0; i < 16; i++) {
      lcdWriteChar((uint8_t)r2[i]);
    }
    lcdLastRow2 = r2;
  }
}

uint8_t scanI2CBusOnPins(int sdaPin, int sclPin) {
  Wire.end();
  delay(10);
  Wire.begin(sdaPin, sclPin);
  Wire.setClock(50000);
  delay(20);

  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      return addr;
    }
  }
  return 0;
}

bool initI2CLcd() {
  int usedSda = LCD_SDA_PIN;
  int usedScl = LCD_SCL_PIN;
  uint8_t detectedAddr = scanI2CBusOnPins(LCD_SDA_PIN, LCD_SCL_PIN);

  // Automatic fallback if SDA/SCL wires were swapped
  if (detectedAddr == 0) {
    detectedAddr = scanI2CBusOnPins(LCD_SCL_PIN, LCD_SDA_PIN);
    if (detectedAddr != 0) {
      usedSda = LCD_SCL_PIN;
      usedScl = LCD_SDA_PIN;
    }
  }

  if (detectedAddr == 0) {
    Serial.println("[LCD] Not detected on I2C (SDA=13, SCL=3) — continuing headless.");
    return false;
  }

  lcdBeginHardware(detectedAddr);
  lcdAvailable = true;
  writeLcdLines("ES-SEE v2 Node", "Connecting...");
  Serial.printf("[LCD] Initialized 16x2 at 0x%02X (SDA=%d, SCL=%d)\n", detectedAddr, usedSda, usedScl);
  return true;
}

// ==========================================
// 4. NON-BLOCKING MM-FMD LOW-LEVEL BUZZER
// ==========================================
bool initBuzzer() {
  // Release UART0 TX so GPIO 1 (U0T) can be probed and driven cleanly
  Serial.flush();
  Serial.end();
  delay(20);

  pinMode(BUZZER_PIN, INPUT_PULLUP);
  delayMicroseconds(500);
  int highState = digitalRead(BUZZER_PIN);

  pinMode(BUZZER_PIN, INPUT_PULLDOWN);
  delayMicroseconds(500);
  int lowState = digitalRead(BUZZER_PIN);

  // Re-open TX-only Serial (RX = -1 keeps GPIO 3 dedicated to I2C SCL)
  Serial.begin(115200, SERIAL_8N1, -1, 1);
  delay(20);

  if (highState == LOW || lowState == HIGH) {
    buzzerAvailable = true;
    Serial.println("[BUZZER] MM-FMD Low-Level Buzzer detected on GPIO 1 (U0T).");
  } else {
    buzzerAvailable = false;
    Serial.println("[BUZZER] Not detected on GPIO 1 (U0T) — continuing silent.");
  }

  Serial.flush();
  Serial.end();
  delay(10);

  if (buzzerAvailable) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, BUZZER_IDLE_LEVEL);
  } else {
    pinMode(BUZZER_PIN, INPUT);
  }
  return buzzerAvailable;
}

void triggerBuzzerPattern(int count, unsigned long onDurationMs, unsigned long offDurationMs) {
  if (!buzzerAvailable || count <= 0) return;
  buzzerRemainingBeeps = count;
  buzzerOnMs = onDurationMs;
  buzzerOffMs = offDurationMs;
  buzzerSounding = true;
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
  buzzerNextToggleMs = millis() + buzzerOnMs;
}

void serviceBuzzer() {
  if (!buzzerAvailable) return;
  if (buzzerRemainingBeeps <= 0 && !buzzerSounding) return;
  unsigned long now = millis();
  if (now < buzzerNextToggleMs) return;

  if (buzzerSounding) {
    digitalWrite(BUZZER_PIN, BUZZER_IDLE_LEVEL);
    buzzerSounding = false;
    buzzerRemainingBeeps--;
    if (buzzerRemainingBeeps > 0) {
      buzzerNextToggleMs = now + buzzerOffMs;
    }
  } else if (buzzerRemainingBeeps > 0) {
    digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
    buzzerSounding = true;
    buzzerNextToggleMs = now + buzzerOnMs;
  }
}

void showLcdScanAlert(const String& row1, const String& row2, unsigned long holdMs = 4000) {
  lcdAlertActive = true;
  lcdAlertUntilMs = millis() + holdMs;
  writeLcdLines(row1, row2);
}

void handleScanFeedback(const String& status, const String& studentName, const String& subjectCode, const String& attStatus) {
  String dispName = studentName.length() > 0 ? studentName : "Student";
  String subjTag = subjectCode.length() > 0 ? subjectCode : "CLASS";

  if (status == "LOGGED") {
    if (attStatus == "LATE") {
      showLcdScanAlert("LATE - " + subjTag, dispName, 4000);
      triggerBuzzerPattern(2, 120, 90);
    } else {
      showLcdScanAlert("PRESENT - " + subjTag, dispName, 4000);
      triggerBuzzerPattern(1, 160, 0);
    }
  } else if (status == "ALREADY_LOGGED") {
    showLcdScanAlert("ALREADY LOGGED", dispName, 3500);
    triggerBuzzerPattern(2, 75, 75);
  } else if (status == "NO_CLASS") {
    showLcdScanAlert("NO ACTIVE CLASS", dispName, 3500);
    triggerBuzzerPattern(2, 75, 75);
  } else if (status == "UNKNOWN" || status == "SPOOF") {
    showLcdScanAlert("ACCESS DENIED", status == "SPOOF" ? "Spoof Blocked" : "Unknown Face", 3500);
    triggerBuzzerPattern(3, 100, 75);
  }
}

// ==========================================
// 5. CLOCK, TIMESTAMP & SD DIRECTORY HELPERS
// ==========================================
void keepFlashGroundedOff() {
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
}

void readClockSnapshot(String& dateIsoOut, String& timeFileOut, String& lcdLine2Out) {
  struct tm tinfo;
  if (getLocalTime(&tinfo, 35)) {
    char dIso[16], tFile[16], dayShort[8], monShort[8], dayNum[4], time12[12];
    strftime(dIso, sizeof(dIso), "%Y-%m-%d", &tinfo);
    strftime(tFile, sizeof(tFile), "%I-%M-%S%p", &tinfo);
    strftime(dayShort, sizeof(dayShort), "%a", &tinfo);
    strftime(monShort, sizeof(monShort), "%b", &tinfo);
    strftime(dayNum, sizeof(dayNum), "%d", &tinfo);

    int hr = tinfo.tm_hour % 12;
    if (hr == 0) hr = 12;
    const char* ampm = (tinfo.tm_hour >= 12) ? "PM" : "AM";
    snprintf(time12, sizeof(time12), "%d:%02d%s", hr, tinfo.tm_min, ampm);

    // 16-char fit: "Sat Nov21 2:27PM"
    lcdLine2Out = String(dayShort) + " " + String(monShort) + String(dayNum) + " " + String(time12);
    dateIsoOut = String(dIso);
    timeFileOut = String(tFile);
  } else {
    char idxBuf[16];
    snprintf(idxBuf, sizeof(idxBuf), "SEQ-%04lu", (unsigned long)bufferCounter++);
    dateIsoOut = "0000-00-00";
    timeFileOut = String(idxBuf);
    lcdLine2Out = "Syncing Clock...";
  }
}

void refreshIdleLcdDisplay() {
  if (!lcdAvailable) return;
  if (lcdAlertActive) {
    if ((long)(millis() - lcdAlertUntilMs) >= 0) {
      lcdAlertActive = false;
    } else {
      return;
    }
  }

  String dateIso, timeFile, lcdLine2;
  readClockSnapshot(dateIso, timeFile, lcdLine2);
  String topRow = serverReachable ? currentClassStatus : "Offline Standby";
  writeLcdLines(topRow, lcdLine2);
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
// 6. STRICT OFFLINE MOTION GATE
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
// 7. CAMERA & MICROSD INITIALIZATION
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

  bool hasPsram = psramFound();
  if (hasPsram) {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("[CAM] PSRAM not detected — using DRAM VGA buffer.");
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK && !hasPsram) {
    config.frame_size   = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    err = esp_camera_init(&config);
  }

  if (err != ESP_OK) {
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    if (s->id.PID == OV3660_PID) {
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
    if (hasPsram) {
      s->set_framesize(s, FRAMESIZE_VGA);
    }
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
  ensureDir("/captured_proofs");
  ensureDir("/attendance_logs");
  ensureDir("/offline_buffer");
  Serial.println("[SD] Ready (/captured_proofs, /attendance_logs, /enrolled_faces).");
  return true;
}

// ==========================================
// 8. OFFLINE BUFFERING & AUTOMATIC CLEANUP
// ==========================================
void bufferFrameOnlyWhenOffline(camera_fb_t* fb, const String& dateIso, const String& timeFile) {
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

  String fullPath = "/offline_buffer/BUF_" + dateIso + "_" + timeFile + ".jpg";
  File img = SD_MMC.open(fullPath, FILE_WRITE);
  if (img) {
    img.write(fb->buf, fb->len);
    img.flush();
    img.close();
    lastOfflineSaveMs = millis();
    showLcdScanAlert("OFFLINE SAVED", timeFile, 2500);
    triggerBuzzerPattern(1, 90, 0);
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

        if (code == 200) {
          SD_MMC.remove(entryPath);
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
// 9. FULL 1:1 MICROSD SYNC & SCAN TELEMETRY
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
  dl.setTimeout(4500);

  int code = dl.GET();
  if (code == 200) {
    if (SD_MMC.exists(sdPath)) {
      SD_MMC.remove(sdPath);
    }
    File out = SD_MMC.open(sdPath, FILE_WRITE);
    if (out) {
      dl.writeToStream(&out);
      out.flush();
      out.close();
      dl.end();
      keepFlashGroundedOff();
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
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/sync_tasks");
  http.setTimeout(2500);

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      const char* clsStatus = doc["class_status"] | "";
      if (strlen(clsStatus) > 0) {
        currentClassStatus = String(clsStatus);
      }

      JsonObject lastScan = doc["last_scan"];
      if (!lastScan.isNull()) {
        int seq = lastScan["seq"] | 0;
        if (seq > 0 && lastHandledScanSeq == 0) {
          lastHandledScanSeq = seq;
        } else if (seq > lastHandledScanSeq) {
          lastHandledScanSeq = seq;
          String sStatus   = lastScan["status"] | "";
          String sName     = lastScan["student_name"] | "";
          String sSubj     = lastScan["subject_code"] | "";
          String sAtt      = lastScan["attendance_status"] | "";
          handleScanFeedback(sStatus, sName, sSubj, sAtt);
        }
      }

      if (sdAvailable) {
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
            }
            ok = true;
          }

          if (ok) {
            acknowledgeSyncTask(taskId);
          }
        }
      }
    }
  }
  http.end();
  keepFlashGroundedOff();
}

void reconcileFullSdManifest() {
  if (!sdAvailable || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_BASE) + "/api/esp32/reconcile_manifest");
  http.setTimeout(3500);

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray files = doc["files"].as<JsonArray>();
      for (JsonObject f : files) {
        String sdPath       = f["sd_path"] | "";
        String localRelPath = f["local_rel_path"] | "";
        bool overwrite      = f["overwrite"] | false;

        if (sdPath.length() == 0 || localRelPath.length() == 0) continue;

        if (overwrite || !SD_MMC.exists(sdPath)) {
          downloadFileFromServerToSd(localRelPath, sdPath);
          serviceBuzzer();
          yield();
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
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      const char* clsStatus = doc["class_status"] | "";
      if (strlen(clsStatus) > 0) {
        currentClassStatus = String(clsStatus);
      }
    }
  }
  http.end();

  bool wasOffline = !serverReachable;
  serverReachable = (code == 200);

  if (serverReachable && wasOffline) {
    flushAndDeleteOfflineBufferToServer();
    processPendingSdSyncTasks();
    reconcileFullSdManifest();
  }
  return serverReachable;
}

// ==========================================
// 10. HIGH-SPEED WEB DASHBOARD STREAMING ENDPOINTS
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
// 11. SETUP & MAIN LOOP
// ==========================================
void setup() {
  keepFlashGroundedOff();

  // TX-only on GPIO 1 (RX = -1) so UART0 never hijacks GPIO 3 (I2C SCL)
  Serial.begin(115200, SERIAL_8N1, -1, 1);
  delay(200);
  Serial.println("\n=== CPE-3B ESP32-CAM v2 (MB102 + Direct I2C LCD + MM-FMD Buzzer) ===");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  // Initialize camera first so DMA/PSRAM gets clean, unfragmented heap
  bool camOk = initCamera();

  // Probe & initialize PCF8574 16x2 LCD directly on locked pins (13, 3)
  initI2CLcd();

  if (!camOk) {
    Serial.println("[FATAL] Camera init failed.");
    writeLcdLines("CAMERA ERROR", "Check OV2640");
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
      processPendingSdSyncTasks();
      reconcileFullSdManifest();
    }
  } else {
    Serial.println("\n[WIFI] Offline — standing by");
  }

  // Probe MM-FMD Low-Level Buzzer on GPIO 1 (U0T) last and hand U0T over from UART
  initBuzzer();
  if (serverReachable) {
    triggerBuzzerPattern(1, 100, 0);
  }

  refreshIdleLcdDisplay();
  keepFlashGroundedOff();
}

void loop() {
  serviceBuzzer();

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

    if (millis() - lastReconcileMs >= RECONCILE_INTERVAL_MS) {
      lastReconcileMs = millis();
      if (serverReachable) {
        reconcileFullSdManifest();
      }
    }
  } else {
    serverReachable = false;
  }

  if (millis() - lastLcdClockMs >= LCD_CLOCK_INTERVAL_MS) {
    lastLcdClockMs = millis();
    refreshIdleLcdDisplay();
  }

  if (!serverReachable && (millis() - lastOfflineCheckMs >= OFFLINE_CHECK_INTERVAL_MS)) {
    lastOfflineCheckMs = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      if (hasSignificantOfflineMotion(fb)) {
        String dateIso, timeFile, lcdLine2;
        readClockSnapshot(dateIso, timeFile, lcdLine2);
        bufferFrameOnlyWhenOffline(fb, dateIso, timeFile);
      }
      esp_camera_fb_return(fb);
    }
  }
}