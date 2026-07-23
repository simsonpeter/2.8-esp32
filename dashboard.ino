/*
 * TC Chat — Free AI chatbot for ES3C28P 2.8" ESP32-S3 (320x240 landscape)
 * Default: anonymous Pollinations text API (no key).
 * Optional: free Groq key (console.groq.com) for faster/reliable replies.
 * Open as: Documents/Arduino/dashboard/dashboard.ino
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2C_SDA       16
#define I2C_SCL       15
#define SCR_W         320
#define SCR_H         240
#define TOUCH_RST     18
#define TOUCH_INT     17
#define TOUCH_ADDR    0x38
#define WIFI_TIMEOUT_MS 30000
#define SPLASH_MS     2500
#define MAX_WIFI_NETS 16
#define WIFI_SSID_LEN 33
#define WIFI_PASS_LEN 64
#define WIFI_LIST_Y0  52
#define WIFI_ROW_H    28
#define WIFI_VISIBLE  4
#define BAT_ADC_PIN   9
#define BAT_DIVIDER   2.0f
#define BAT_SAMPLES   8
#define BAT_UPDATE_MS 2000
#define BAT_ANIM_MS   450
#define BAT_ICON_X    14
#define BAT_ICON_Y    10
#define BAT_ICON_W    36
#define BAT_ICON_H    18
#define BAT_HISTORY   8
#define BAT_NO_BAT_V  4.30f

#define MAX_MSGS      8
#define MSG_LEN       180
#define DRAFT_LEN     96
#define PROMPT_BUF    1400
#define REPLY_BUF     512
#define API_KEY_LEN   128
#define AI_URL_BASE   "https://text.pollinations.ai/"
#define GROQ_URL      "https://api.groq.com/openai/v1/chat/completions"
#define GROQ_MODEL    "llama-3.1-8b-instant"

// Chat chrome
#define HEADER_H      30
#define STATUS_H      18
#define INPUT_H       28
#define MSG_AREA_Y0   (HEADER_H + 4)
#define MSG_AREA_Y1_KB 98
#define MSG_AREA_Y1_FULL (SCR_H - INPUT_H - 8)
#define INPUT_Y_KB    100
#define INPUT_Y_FULL  (SCR_H - INPUT_H - 4)
#define KB_Y0         130

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341  _panel_instance;
  lgfx::Bus_SPI        _bus_instance;
  lgfx::Light_PWM      _light_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc = 46;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 45;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

static LGFX lcd;

struct ChatMsg {
  bool fromUser;
  char text[MSG_LEN];
};

ChatMsg messages[MAX_MSGS];
int msgCount = 0;
int msgScroll = 0;
char draft[DRAFT_LEN] = "";
char statusLine[48] = "Starting...";
bool chatDirty = true;
bool kbShift = false;
bool aiBusy = false;
volatile bool aiRequestPending = false;
volatile bool aiReplyReady = false;
char aiPendingPrompt[PROMPT_BUF];
char aiReplyText[REPLY_BUF];
char aiErrorText[64] = "";

char wifiSsid[WIFI_SSID_LEN] = "simson";
char wifiPass[WIFI_PASS_LEN] = "jayatha10";

enum BootState { BOOT_SPLASH, BOOT_WIFI, BOOT_DONE };
enum UiMode { UI_CHAT, UI_COMPOSE, UI_WIFI, UI_WIFI_PASS, UI_API_KEY };
BootState bootState = BOOT_SPLASH;
UiMode uiMode = UI_CHAT;
char groqApiKey[API_KEY_LEN] = "";
char apiKeyDraft[API_KEY_LEN] = "";
bool apiKbShift = false;
unsigned long wifiStartMs = 0;
unsigned long splashStartMs = 0;
Preferences wifiPrefs;
bool wifiDirty = true;
bool wifiScanning = false;
bool wifiConnecting = false;
bool wifiConnectFailed = false;
unsigned long wifiConnectStartMs = 0;
int wifiNetCount = 0;
int wifiSelected = 0;
int wifiScroll = 0;
char wifiFoundSsid[MAX_WIFI_NETS][WIFI_SSID_LEN];
int32_t wifiFoundRssi[MAX_WIFI_NETS];
uint8_t wifiFoundEnc[MAX_WIFI_NETS];
char wifiPassDraft[WIFI_PASS_LEN] = "";
bool wifiKbShift = false;
const char* wifiKbRows[] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm"
};
const char* wifiKbRowsShift[] = {
  "!@#$%^&*()",
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM"
};

float batVoltage = 0.0f;
int batPercent = 0;
bool batPresent = false;
bool batCharging = false;
bool batFull = false;
bool batDirty = true;
uint8_t batAnimFrame = 0;
unsigned long lastBatSampleMs = 0;
unsigned long lastBatAnimMs = 0;
int batDisplayPct = -1;
float batHistory[BAT_HISTORY];
int batHistoryIdx = 0;
int batHistoryCount = 0;

unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 180;
TaskHandle_t aiTaskHandle = nullptr;
SemaphoreHandle_t aiMutex = nullptr;

uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); }

void drawBackground();
void showSplashScreen();
void showBootError(const char* line1, const char* line2);
void setStatus(const char* msg);
void initTouchController();
bool readTouchScreen(int32_t &sx, int32_t &sy);
void handleTouch(int32_t tx, int32_t ty);
void initBatteryMonitor();
void updateBatteryMonitor();
float readBatteryVoltage();
int batteryVoltageToPercent(float volts);
void drawBatteryMeter(int x, int y);
void drawBatteryBolt(int cx, int cy, uint16_t col);
void loadWifiCredentials();
void saveWifiCredentials();
void loadApiKey();
void saveApiKey();
void openWifiTool();
void closeWifiTool();
void openApiKeyTool();
void closeApiKeyTool();
void drawApiKeyScreen();
void handleApiKeyTouch(int32_t tx, int32_t ty);
void appendApiKeyChar(char c);
void backspaceApiKey();
void startWifiScan();
void pollWifiScan();
void drawWifiScreen();
void drawWifiPassScreen();
void handleWifiTouch(int32_t tx, int32_t ty);
void handleWifiPassTouch(int32_t tx, int32_t ty);
void connectSelectedWifi();
void beginWifiConnect(const char* ssid, const char* pass);
void pollWifiConnect();
void appendWifiPassChar(char c);
void backspaceWifiPass();
void joinWifiWithPassword();
bool wifiNeedsPassword(int idx);
void drawChatScreen();
void drawComposeKeyboard();
void drawHeader();
void drawMessageArea();
void drawInputBar(bool composing);
void handleChatTouch(int32_t tx, int32_t ty);
void handleComposeTouch(int32_t tx, int32_t ty);
void appendDraftChar(char c);
void backspaceDraft();
void clearDraft();
void addMessage(bool fromUser, const char* text);
void sendDraftMessage();
void buildAiPrompt(char* out, size_t outLen);
void urlEncode(const char* src, char* dest, size_t destLen);
bool extractJsonContent(const char* json, char* dest, size_t destLen);
bool callGroqAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen);
bool callPollinationsAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen);
bool callFreeAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen);
void aiWorkerTask(void* param);
void startAiTask();
void requestAiReply();
void applyAiReply();
void wrapDrawText(const char* text, int x, int y, int maxW, int maxLines, uint16_t color);

void showSplashScreen() {
  lcd.fillScreen(TFT_WHITE);
  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(3);
  const char* title = "TC CHAT";
  int tw = lcd.textWidth(title);
  lcd.drawString(title, (SCR_W - tw) / 2, 88);
  lcd.setTextSize(1);
  lcd.setTextColor(C(120, 120, 140));
  const char* sub = "Free AI chatbot";
  tw = lcd.textWidth(sub);
  lcd.drawString(sub, (SCR_W - tw) / 2, 130);
}

void showBootError(const char* line1, const char* line2) {
  lcd.fillRect(20, 90, 280, 60, TFT_BLACK);
  lcd.setTextColor(TFT_RED);
  lcd.setTextSize(2);
  lcd.drawString(line1, 30, 95);
  lcd.setTextSize(1);
  lcd.drawString(line2, 30, 120);
}

void drawBackground() {
  for (int y = 0; y < SCR_H; y++) {
    uint8_t t = (y * 255) / (SCR_H - 1);
    uint8_t r = 108 - (t * 40 / 255);
    uint8_t g = 92 + (t * 40 / 255);
    uint8_t b = 231 - (t * 4 / 255);
    lcd.drawFastHLine(0, y, SCR_W, C(r, g, b));
  }
}

void setStatus(const char* msg) {
  strncpy(statusLine, msg, sizeof(statusLine) - 1);
  statusLine[sizeof(statusLine) - 1] = '\0';
  chatDirty = true;
}

void initTouchController() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(300);
  pinMode(TOUCH_INT, INPUT);
  Wire.begin(I2C_SDA, I2C_SCL);
}

bool readTouchScreen(int32_t &sx, int32_t &sy) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)1) != 1) return false;
  uint8_t touches = Wire.read();
  if (touches == 0 || touches > 2) return false;

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x03);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)4) != 4) return false;
  uint8_t d[4];
  for (int i = 0; i < 4; i++) d[i] = Wire.read();

  int16_t raw_x = ((d[0] & 0x0F) << 8) | d[1];
  int16_t raw_y = ((d[2] & 0x0F) << 8) | d[3];
  sx = (SCR_W - 1) - raw_y;
  sy = raw_x;
  return (sx >= 0 && sx < SCR_W && sy >= 0 && sy < SCR_H);
}

void initBatteryMonitor() {
  pinMode(BAT_ADC_PIN, INPUT);
  analogReadResolution(12);
#if defined(analogSetPinAttenuation)
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
#endif
  for (int i = 0; i < BAT_HISTORY; i++) batHistory[i] = 0.0f;
  batVoltage = readBatteryVoltage();
  batPercent = batteryVoltageToPercent(batVoltage);
  batPresent = batVoltage > 2.5f && batVoltage < BAT_NO_BAT_V;
  batDisplayPct = batPercent;
  batDirty = true;
}

float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  return (sum / (float)BAT_SAMPLES) * BAT_DIVIDER / 1000.0f;
}

int batteryVoltageToPercent(float volts) {
  static const float curveV[] = {4.10f, 4.05f, 3.98f, 3.92f, 3.87f, 3.82f, 3.79f, 3.77f, 3.74f, 3.68f, 3.40f};
  static const int curveP[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0};
  const int n = 11;
  if (volts >= curveV[0]) return 100;
  if (volts <= curveV[n - 1]) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (volts <= curveV[i] && volts >= curveV[i + 1]) {
      float t = (volts - curveV[i + 1]) / (curveV[i] - curveV[i + 1]);
      return (int)(curveP[i + 1] + t * (curveP[i] - curveP[i + 1]) + 0.5f);
    }
  }
  return 0;
}

void updateBatteryMonitor() {
  if (millis() - lastBatSampleMs < BAT_UPDATE_MS) return;
  lastBatSampleMs = millis();

  float sample = readBatteryVoltage();
  batVoltage = (batVoltage <= 0.0f) ? sample : (batVoltage * 0.65f + sample * 0.35f);
  batHistory[batHistoryIdx] = batVoltage;
  batHistoryIdx = (batHistoryIdx + 1) % BAT_HISTORY;
  if (batHistoryCount < BAT_HISTORY) batHistoryCount++;

  bool wasPresent = batPresent;
  bool wasCharging = batCharging;
  int wasPercent = batPercent;

  batPresent = batVoltage > 2.5f && batVoltage < BAT_NO_BAT_V;
  if (!batPresent) {
    batPercent = 100;
    batCharging = batVoltage >= BAT_NO_BAT_V;
    batFull = !batCharging;
  } else {
    batPercent = batteryVoltageToPercent(batVoltage);
    float oldest = batHistory[batHistoryIdx];
    float delta = batVoltage - oldest;
    batFull = batVoltage >= 4.10f && delta <= 0.008f;
    batCharging = !batFull && batPercent < 98 && delta > 0.012f;
  }

  if (batPresent != wasPresent || batCharging != wasCharging ||
      abs(batPercent - wasPercent) >= 2 || abs(batPercent - batDisplayPct) >= 1) {
    batDirty = true;
  }
}

void drawBatteryBolt(int cx, int cy, uint16_t col) {
  lcd.fillTriangle(cx - 1, cy - 5, cx + 3, cy - 5, cx, cy + 1, col);
  lcd.fillTriangle(cx - 3, cy + 5, cx + 1, cy + 5, cx, cy - 1, col);
}

void drawBatteryMeter(int x, int y) {
  const int bodyW = 24;
  const int bodyH = 12;
  const int capW = 3;
  const int capH = 6;

  uint16_t frameCol = C(120, 120, 140);
  uint16_t fillCol = C(0, 184, 148);
  int pct = batPercent;
  if (!batPresent) {
    pct = 100;
    fillCol = C(116, 185, 255);
  } else if (pct <= 20) {
    fillCol = C(255, 118, 117);
  } else if (pct <= 50) {
    fillCol = C(253, 150, 68);
  }

  lcd.drawRoundRect(x, y + 3, bodyW, bodyH, 3, frameCol);
  lcd.fillRect(x + bodyW, y + 6, capW, capH, frameCol);

  int innerW = bodyW - 4;
  int fillW = (pct * innerW) / 100;
  if (fillW > 0) lcd.fillRoundRect(x + 2, y + 5, fillW, bodyH - 4, 2, fillCol);
  if (batCharging && batAnimFrame) drawBatteryBolt(x + 12, y + 9, C(253, 221, 68));
  batDisplayPct = batPercent;
  batDirty = false;
}

void loadWifiCredentials() {
  wifiPrefs.begin("wifi", true);
  String s = wifiPrefs.getString("ssid", wifiSsid);
  String p = wifiPrefs.getString("pass", wifiPass);
  wifiPrefs.end();
  strncpy(wifiSsid, s.c_str(), WIFI_SSID_LEN - 1);
  wifiSsid[WIFI_SSID_LEN - 1] = '\0';
  strncpy(wifiPass, p.c_str(), WIFI_PASS_LEN - 1);
  wifiPass[WIFI_PASS_LEN - 1] = '\0';
}

void saveWifiCredentials() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", wifiSsid);
  wifiPrefs.putString("pass", wifiPass);
  wifiPrefs.end();
}

void loadApiKey() {
  wifiPrefs.begin("ai", true);
  String k = wifiPrefs.getString("groq", "");
  wifiPrefs.end();
  strncpy(groqApiKey, k.c_str(), API_KEY_LEN - 1);
  groqApiKey[API_KEY_LEN - 1] = '\0';
}

void saveApiKey() {
  wifiPrefs.begin("ai", false);
  wifiPrefs.putString("groq", groqApiKey);
  wifiPrefs.end();
}

void openWifiTool() {
  uiMode = UI_WIFI;
  wifiDirty = true;
  wifiConnecting = false;
  wifiConnectFailed = false;
  wifiConnectStartMs = 0;
  wifiPassDraft[0] = '\0';
  wifiKbShift = false;
  startWifiScan();
}

void closeWifiTool() {
  uiMode = UI_CHAT;
  chatDirty = true;
  wifiDirty = false;
  wifiScanning = false;
  wifiConnecting = false;
  if (WiFi.status() == WL_CONNECTED) setStatus("Online — ask me anything");
  else setStatus("Offline — connect WiFi");
}

void openApiKeyTool() {
  uiMode = UI_API_KEY;
  strncpy(apiKeyDraft, groqApiKey, API_KEY_LEN - 1);
  apiKeyDraft[API_KEY_LEN - 1] = '\0';
  apiKbShift = false;
  chatDirty = true;
}

void closeApiKeyTool() {
  uiMode = UI_CHAT;
  chatDirty = true;
}

void appendApiKeyChar(char c) {
  size_t len = strlen(apiKeyDraft);
  if (len >= API_KEY_LEN - 1) return;
  apiKeyDraft[len] = c;
  apiKeyDraft[len + 1] = '\0';
  chatDirty = true;
}

void backspaceApiKey() {
  size_t len = strlen(apiKeyDraft);
  if (len == 0) return;
  apiKeyDraft[len - 1] = '\0';
  chatDirty = true;
}

void drawApiKeyScreen() {
  drawBackground();
  lcd.fillRoundRect(8, 6, SCR_W - 16, SCR_H - 12, 14, C(255, 255, 255));

  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(1);
  lcd.drawString("Free Groq API key (console.groq.com)", 14, 10);
  drawBatteryMeter(270, 8);

  lcd.fillRoundRect(14, 28, 240, 26, 8, C(245, 247, 255));
  lcd.setTextColor(C(40, 40, 60));
  if (apiKeyDraft[0]) {
    char shown[40];
    size_t n = strlen(apiKeyDraft);
    if (n <= 18) {
      for (size_t i = 0; i < n && i < sizeof(shown) - 1; i++) shown[i] = '*';
      shown[n] = '\0';
    } else {
      snprintf(shown, sizeof(shown), "********...%s", apiKeyDraft + n - 6);
    }
    lcd.drawString(shown, 22, 36);
  } else {
    lcd.setTextColor(C(150, 150, 170));
    lcd.drawString("(optional — blank = free no-key AI)", 22, 36);
  }

  lcd.fillRoundRect(260, 28, 44, 26, 8, C(255, 118, 117));
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Del", 272, 36);

  const int keyW = 28;
  const int keyH = 24;
  int startY = 62;
  for (int row = 0; row < 4; row++) {
    const char* keys = apiKbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    for (int i = 0; i < len; i++) {
      char label[2] = { keys[i], '\0' };
      int x = startX + i * keyW;
      int y = startY + row * (keyH + 4);
      lcd.fillRoundRect(x, y, keyW - 2, keyH, 5, C(230, 235, 255));
      lcd.setTextColor(C(40, 40, 60));
      lcd.drawString(label, x + 9, y + 7);
    }
  }

  lcd.fillRoundRect(14, 178, 56, 28, 8, C(116, 185, 255));
  lcd.fillRoundRect(80, 178, 56, 28, 8, C(162, 155, 254));
  lcd.fillRoundRect(146, 178, 70, 28, 8, C(200, 210, 255));
  lcd.fillRoundRect(226, 178, 78, 28, 8, C(0, 184, 148));
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Back", 28, 188);
  lcd.drawString("Shift", 90, 188);
  lcd.setTextColor(C(40, 40, 60));
  lcd.drawString("Clear", 160, 188);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Save", 250, 188);
}

void handleApiKeyTouch(int32_t tx, int32_t ty) {
  if (ty >= 28 && ty <= 54 && tx >= 260 && tx <= 304) {
    backspaceApiKey();
    return;
  }

  const int keyW = 28;
  const int keyH = 24;
  int startY = 62;
  for (int row = 0; row < 4; row++) {
    const char* keys = apiKbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    int y = startY + row * (keyH + 4);
    if (ty < y || ty >= y + keyH) continue;
    for (int i = 0; i < len; i++) {
      int x = startX + i * keyW;
      if (tx >= x && tx < x + keyW - 2) {
        appendApiKeyChar(keys[i]);
        return;
      }
    }
  }

  if (ty >= 178 && ty <= 206) {
    if (tx >= 14 && tx <= 70) {
      closeApiKeyTool();
      return;
    }
    if (tx >= 80 && tx <= 136) {
      apiKbShift = !apiKbShift;
      chatDirty = true;
      return;
    }
    if (tx >= 146 && tx <= 216) {
      apiKeyDraft[0] = '\0';
      chatDirty = true;
      return;
    }
    if (tx >= 226 && tx <= 304) {
      strncpy(groqApiKey, apiKeyDraft, API_KEY_LEN - 1);
      groqApiKey[API_KEY_LEN - 1] = '\0';
      saveApiKey();
      setStatus(groqApiKey[0] ? "Groq key saved" : "Using free no-key AI");
      closeApiKeyTool();
    }
  }
}

bool wifiNeedsPassword(int idx) {
  if (idx < 0 || idx >= wifiNetCount) return true;
  return wifiFoundEnc[idx] != WIFI_AUTH_OPEN;
}

void startWifiScan() {
  wifiScanning = true;
  wifiNetCount = 0;
  wifiSelected = 0;
  wifiScroll = 0;
  wifiDirty = true;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}

void pollWifiScan() {
  if (!wifiScanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  wifiScanning = false;
  wifiNetCount = 0;
  if (n < 0) n = 0;
  if (n > MAX_WIFI_NETS) n = MAX_WIFI_NETS;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    strncpy(wifiFoundSsid[i], s.c_str(), WIFI_SSID_LEN - 1);
    wifiFoundSsid[i][WIFI_SSID_LEN - 1] = '\0';
    wifiFoundRssi[i] = WiFi.RSSI(i);
    wifiFoundEnc[i] = WiFi.encryptionType(i);
    wifiNetCount++;
  }
  WiFi.scanDelete();
  if (wifiSelected >= wifiNetCount) wifiSelected = 0;
  wifiDirty = true;
}

void beginWifiConnect(const char* ssid, const char* pass) {
  strncpy(wifiSsid, ssid, WIFI_SSID_LEN - 1);
  wifiSsid[WIFI_SSID_LEN - 1] = '\0';
  strncpy(wifiPass, pass, WIFI_PASS_LEN - 1);
  wifiPass[WIFI_PASS_LEN - 1] = '\0';
  wifiConnecting = true;
  wifiConnectFailed = false;
  wifiConnectStartMs = millis();
  wifiDirty = true;
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.mode(WIFI_STA);
  if (pass && pass[0]) WiFi.begin(wifiSsid, wifiPass);
  else WiFi.begin(wifiSsid);
}

void pollWifiConnect() {
  if (!wifiConnecting) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    wifiConnectFailed = false;
    WiFi.setSleep(false);
    saveWifiCredentials();
    setStatus("WiFi connected");
    closeWifiTool();
    return;
  }
  if (millis() - wifiConnectStartMs > WIFI_TIMEOUT_MS) {
    wifiConnecting = false;
    wifiConnectFailed = true;
    wifiDirty = true;
    if (uiMode == UI_WIFI_PASS) uiMode = UI_WIFI;
  }
}

void connectSelectedWifi() {
  if (wifiNetCount <= 0 || wifiSelected < 0 || wifiSelected >= wifiNetCount) return;
  if (!wifiNeedsPassword(wifiSelected)) {
    beginWifiConnect(wifiFoundSsid[wifiSelected], "");
    return;
  }
  if (strcmp(wifiFoundSsid[wifiSelected], wifiSsid) == 0 && wifiPass[0]) {
    beginWifiConnect(wifiFoundSsid[wifiSelected], wifiPass);
    return;
  }
  wifiPassDraft[0] = '\0';
  wifiKbShift = false;
  uiMode = UI_WIFI_PASS;
  wifiDirty = true;
}

void appendWifiPassChar(char c) {
  size_t len = strlen(wifiPassDraft);
  if (len >= WIFI_PASS_LEN - 1) return;
  wifiPassDraft[len] = c;
  wifiPassDraft[len + 1] = '\0';
  wifiDirty = true;
}

void backspaceWifiPass() {
  size_t len = strlen(wifiPassDraft);
  if (len == 0) return;
  wifiPassDraft[len - 1] = '\0';
  wifiDirty = true;
}

void joinWifiWithPassword() {
  if (wifiSelected < 0 || wifiSelected >= wifiNetCount) return;
  beginWifiConnect(wifiFoundSsid[wifiSelected], wifiPassDraft);
}

void drawWifiScreen() {
  drawBackground();
  lcd.fillRoundRect(10, 8, SCR_W - 20, SCR_H - 16, 16, C(255, 255, 255));
  lcd.drawRoundRect(10, 8, SCR_W - 20, SCR_H - 16, 16, C(200, 210, 255));

  lcd.fillRoundRect(18, 16, 56, 24, 8, C(116, 185, 255));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1);
  lcd.drawString("Back", 32, 24);

  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(2);
  lcd.drawString("WiFi", 130, 18);
  drawBatteryMeter(82, 16);

  lcd.fillRoundRect(246, 16, 56, 24, 8, C(0, 184, 148));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1);
  lcd.drawString("Scan", 260, 24);

  lcd.fillRect(18, 46, SCR_W - 36, 14, C(255, 255, 255));
  lcd.setTextColor(C(100, 100, 120));
  lcd.setTextSize(1);
  if (wifiConnecting) lcd.drawString("Connecting...", 20, 46);
  else if (wifiScanning) lcd.drawString("Scanning...", 20, 46);
  else if (WiFi.status() == WL_CONNECTED) {
    char cur[48];
    snprintf(cur, sizeof(cur), "Connected: %s", wifiSsid);
    lcd.drawString(cur, 20, 46);
  } else if (wifiNetCount == 0) {
    lcd.drawString("No networks — tap Scan", 20, 46);
  } else {
    char cur[48];
    snprintf(cur, sizeof(cur), "Saved: %s", wifiSsid);
    lcd.drawString(cur, 20, 46);
  }

  int y = WIFI_LIST_Y0;
  for (int i = 0; i < WIFI_VISIBLE; i++) {
    int idx = wifiScroll + i;
    lcd.fillRoundRect(18, y, SCR_W - 36, WIFI_ROW_H - 4, 8,
                       (idx == wifiSelected) ? C(230, 235, 255) : C(245, 247, 255));
    if (idx < wifiNetCount) {
      lcd.setTextColor((idx == wifiSelected) ? C(108, 92, 231) : C(40, 40, 60));
      lcd.setTextSize(1);
      lcd.drawString(wifiFoundSsid[idx], 26, y + 8);
      char meta[16];
      snprintf(meta, sizeof(meta), "%ddBm%s", (int)wifiFoundRssi[idx],
               wifiNeedsPassword(idx) ? " *" : "");
      lcd.setTextColor(C(120, 120, 140));
      int tw = lcd.textWidth(meta);
      lcd.drawString(meta, SCR_W - 24 - tw, y + 8);
    }
    y += WIFI_ROW_H;
  }

  lcd.fillRoundRect(40, 172, 50, 28, 8, C(116, 185, 255));
  lcd.fillRoundRect(135, 172, 50, 28, 8, C(108, 92, 231));
  lcd.fillRoundRect(230, 172, 50, 28, 8, C(116, 185, 255));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.drawString("<", 58, 178);
  lcd.setTextSize(1);
  lcd.drawString("Join", 148, 182);
  lcd.setTextSize(2);
  lcd.drawString(">", 248, 178);

  if (wifiConnecting) {
    lcd.setTextColor(C(253, 150, 68));
    lcd.setTextSize(1);
    lcd.drawString("Please wait...", 110, 210);
  } else if (wifiConnectFailed) {
    lcd.setTextColor(C(255, 118, 117));
    lcd.setTextSize(1);
    lcd.drawString("Connect failed — try again", 70, 210);
  }
  wifiDirty = false;
}

void drawWifiPassScreen() {
  drawBackground();
  lcd.fillRoundRect(8, 6, SCR_W - 16, SCR_H - 12, 14, C(255, 255, 255));

  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(1);
  char title[48];
  snprintf(title, sizeof(title), "Password: %s",
           (wifiSelected >= 0 && wifiSelected < wifiNetCount) ? wifiFoundSsid[wifiSelected] : "");
  lcd.drawString(title, 16, 12);
  drawBatteryMeter(270, 8);

  lcd.fillRoundRect(14, 28, 240, 26, 8, C(245, 247, 255));
  lcd.setTextColor(C(40, 40, 60));
  char shown[WIFI_PASS_LEN];
  size_t n = strlen(wifiPassDraft);
  for (size_t i = 0; i < n && i < sizeof(shown) - 1; i++) shown[i] = '*';
  shown[n] = '\0';
  lcd.drawString(shown[0] ? shown : "(empty)", 22, 36);

  lcd.fillRoundRect(260, 28, 44, 26, 8, C(255, 118, 117));
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Del", 272, 36);

  const int keyW = 28;
  const int keyH = 24;
  int startY = 62;
  for (int row = 0; row < 4; row++) {
    const char* keys = wifiKbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    for (int i = 0; i < len; i++) {
      char label[2] = { keys[i], '\0' };
      int x = startX + i * keyW;
      int y = startY + row * (keyH + 4);
      lcd.fillRoundRect(x, y, keyW - 2, keyH, 5, C(230, 235, 255));
      lcd.setTextColor(C(40, 40, 60));
      lcd.drawString(label, x + 9, y + 7);
    }
  }

  lcd.fillRoundRect(14, 178, 56, 28, 8, C(116, 185, 255));
  lcd.fillRoundRect(80, 178, 56, 28, 8, C(162, 155, 254));
  lcd.fillRoundRect(146, 178, 70, 28, 8, C(200, 210, 255));
  lcd.fillRoundRect(226, 178, 78, 28, 8, C(0, 184, 148));
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Back", 28, 188);
  lcd.drawString("Shift", 90, 188);
  lcd.setTextColor(C(40, 40, 60));
  lcd.drawString("Space", 160, 188);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString(wifiConnecting ? "..." : "Join", 250, 188);
  wifiDirty = false;
}

void handleWifiTouch(int32_t tx, int32_t ty) {
  if (wifiConnecting) return;
  if (ty >= 16 && ty <= 40 && tx >= 18 && tx <= 74) { closeWifiTool(); return; }
  if (ty >= 16 && ty <= 40 && tx >= 246 && tx <= 302) { startWifiScan(); return; }

  if (ty >= WIFI_LIST_Y0 && ty < WIFI_LIST_Y0 + WIFI_VISIBLE * WIFI_ROW_H) {
    int row = (ty - WIFI_LIST_Y0) / WIFI_ROW_H;
    int idx = wifiScroll + row;
    if (idx >= 0 && idx < wifiNetCount) {
      wifiSelected = idx;
      wifiDirty = true;
    }
    return;
  }

  if (ty >= 172 && ty <= 200) {
    if (tx >= 40 && tx <= 90) {
      if (wifiSelected > 0) {
        wifiSelected--;
        if (wifiSelected < wifiScroll) wifiScroll = wifiSelected;
        wifiDirty = true;
      }
      return;
    }
    if (tx >= 135 && tx <= 185) { connectSelectedWifi(); return; }
    if (tx >= 230 && tx <= 280) {
      if (wifiSelected < wifiNetCount - 1) {
        wifiSelected++;
        if (wifiSelected >= wifiScroll + WIFI_VISIBLE) wifiScroll = wifiSelected - WIFI_VISIBLE + 1;
        wifiDirty = true;
      }
    }
  }
}

void handleWifiPassTouch(int32_t tx, int32_t ty) {
  if (wifiConnecting) return;
  if (ty >= 28 && ty <= 54 && tx >= 260 && tx <= 304) { backspaceWifiPass(); return; }

  const int keyW = 28;
  const int keyH = 24;
  int startY = 62;
  for (int row = 0; row < 4; row++) {
    const char* keys = wifiKbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    int y = startY + row * (keyH + 4);
    if (ty < y || ty >= y + keyH) continue;
    for (int i = 0; i < len; i++) {
      int x = startX + i * keyW;
      if (tx >= x && tx < x + keyW - 2) {
        appendWifiPassChar(keys[i]);
        return;
      }
    }
  }

  if (ty >= 178 && ty <= 206) {
    if (tx >= 14 && tx <= 70) { uiMode = UI_WIFI; wifiDirty = true; return; }
    if (tx >= 80 && tx <= 136) { wifiKbShift = !wifiKbShift; wifiDirty = true; return; }
    if (tx >= 146 && tx <= 216) { appendWifiPassChar(' '); return; }
    if (tx >= 226 && tx <= 304) joinWifiWithPassword();
  }
}

void wrapDrawText(const char* text, int x, int y, int maxW, int maxLines, uint16_t color) {
  if (!text || !text[0] || maxLines <= 0) return;
  lcd.setTextColor(color);
  lcd.setTextSize(1);
  char line[48];
  int lineIdx = 0;
  int linesDrawn = 0;
  const char* p = text;
  while (*p && linesDrawn < maxLines) {
    lineIdx = 0;
    line[0] = '\0';
    while (*p == ' ') p++;
    while (*p && *p != '\n') {
      char trial[49];
      memcpy(trial, line, lineIdx);
      trial[lineIdx] = *p;
      trial[lineIdx + 1] = '\0';
      if (lcd.textWidth(trial) > maxW) break;
      line[lineIdx++] = *p++;
      line[lineIdx] = '\0';
      if (lineIdx >= (int)sizeof(line) - 1) break;
    }
    if (lineIdx == 0 && *p && *p != '\n') {
      line[lineIdx++] = *p++;
      line[lineIdx] = '\0';
    }
    if (linesDrawn == maxLines - 1 && *p && *p != '\n') {
      if (lineIdx > 3) {
        line[lineIdx - 1] = '.';
        line[lineIdx - 2] = '.';
        line[lineIdx - 3] = '.';
      }
      while (*p && *p != '\n') p++;
    }
    lcd.drawString(line, x, y + linesDrawn * 12);
    linesDrawn++;
    if (*p == '\n') p++;
  }
}

void addMessage(bool fromUser, const char* text) {
  if (!text || !text[0]) return;
  if (msgCount >= MAX_MSGS) {
    for (int i = 1; i < MAX_MSGS; i++) messages[i - 1] = messages[i];
    msgCount = MAX_MSGS - 1;
  }
  messages[msgCount].fromUser = fromUser;
  strncpy(messages[msgCount].text, text, MSG_LEN - 1);
  messages[msgCount].text[MSG_LEN - 1] = '\0';
  msgCount++;
  int visible = (uiMode == UI_COMPOSE) ? 3 : 5;
  msgScroll = (msgCount > visible) ? (msgCount - visible) : 0;
  chatDirty = true;
}

void appendDraftChar(char c) {
  size_t len = strlen(draft);
  if (len >= DRAFT_LEN - 1) return;
  draft[len] = c;
  draft[len + 1] = '\0';
  chatDirty = true;
}

void backspaceDraft() {
  size_t len = strlen(draft);
  if (len == 0) return;
  draft[len - 1] = '\0';
  chatDirty = true;
}

void clearDraft() {
  draft[0] = '\0';
  chatDirty = true;
}

void urlEncode(const char* src, char* dest, size_t destLen) {
  static const char* hex = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 4 < destLen; i++) {
    unsigned char c = (unsigned char)src[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dest[o++] = (char)c;
    } else if (c == ' ') {
      dest[o++] = '%'; dest[o++] = '2'; dest[o++] = '0';
    } else {
      dest[o++] = '%';
      dest[o++] = hex[(c >> 4) & 0xF];
      dest[o++] = hex[c & 0xF];
    }
  }
  dest[o] = '\0';
}

void buildAiPrompt(char* out, size_t outLen) {
  snprintf(out, outLen,
           "You are TC Chat, a helpful free AI on a tiny ESP32 touchscreen. "
           "Reply in plain text only. Keep answers under 45 words.\n");
  // Include recent turns for light context (skip oldest if needed)
  int start = 0;
  if (msgCount > 4) start = msgCount - 4;
  for (int i = start; i < msgCount; i++) {
    size_t used = strlen(out);
    if (used + 40 >= outLen) break;
    snprintf(out + used, outLen - used, "%s: %s\n",
             messages[i].fromUser ? "User" : "Assistant", messages[i].text);
  }
  size_t used = strlen(out);
  if (used + 16 < outLen) strncat(out, "Assistant:", outLen - used - 1);
}

bool extractJsonContent(const char* json, char* dest, size_t destLen) {
  dest[0] = '\0';
  if (!json) return false;
  const char* key = strstr(json, "\"content\"");
  if (!key) return false;
  key += 9;
  while (*key == ' ' || *key == '\t' || *key == ':' ) key++;
  if (*key != '"') return false;
  key++;
  size_t i = 0;
  while (*key && *key != '"' && i < destLen - 1) {
    if (*key == '\\' && key[1]) {
      key++;
      if (*key == 'n') dest[i++] = ' ';
      else if (*key == 'r' || *key == 't') dest[i++] = ' ';
      else dest[i++] = *key;
      key++;
      continue;
    }
    dest[i++] = *key++;
  }
  dest[i] = '\0';
  return i > 0;
}

bool callGroqAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen) {
  reply[0] = '\0';
  err[0] = '\0';
  if (!groqApiKey[0]) {
    strncpy(err, "No Groq key", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }

  // Escape prompt for JSON
  String esc;
  esc.reserve(strlen(prompt) + 16);
  for (const char* p = prompt; *p; p++) {
    char c = *p;
    if (c == '\\' || c == '"') { esc += '\\'; esc += c; }
    else if (c == '\n') esc += "\\n";
    else if (c == '\r') continue;
    else esc += c;
  }

  String body = String("{\"model\":\"") + GROQ_MODEL +
                "\",\"temperature\":0.7,\"max_tokens\":120,\"messages\":["
                "{\"role\":\"system\",\"content\":\"You are TC Chat on a tiny ESP32 screen. Plain text only. Under 45 words.\"},"
                "{\"role\":\"user\",\"content\":\"" + esc + "\"}]}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(25000);
  if (!http.begin(client, GROQ_URL)) {
    strncpy(err, "Groq begin fail", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + groqApiKey);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  if (code != HTTP_CODE_OK) {
    snprintf(err, errLen, "Groq HTTP %d", code);
    return false;
  }
  if (!extractJsonContent(resp.c_str(), reply, replyLen)) {
    strncpy(err, "Groq parse fail", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }
  for (char* p = reply; *p; p++) {
    if (*p == '\r' || *p == '\n') *p = ' ';
  }
  return true;
}

bool callPollinationsAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen) {
  reply[0] = '\0';
  err[0] = '\0';

  static char encoded[3600];
  urlEncode(prompt, encoded, sizeof(encoded));
  String url = String(AI_URL_BASE) + encoded;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(25000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    strncpy(err, "HTTP begin fail", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }
  http.addHeader("Accept", "text/plain");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(err, errLen, "HTTP %d", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  body.trim();
  if (body.length() == 0) {
    strncpy(err, "Empty reply", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }
  if (body.startsWith("{") && body.indexOf("\"error\"") >= 0) {
    strncpy(err, "Free API busy", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }
  // Some gateways return OpenAI JSON even on the GET path.
  if (body.indexOf("\"content\"") >= 0) {
    if (extractJsonContent(body.c_str(), reply, replyLen)) {
      for (char* p = reply; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
      return true;
    }
  }
  strncpy(reply, body.c_str(), replyLen - 1);
  reply[replyLen - 1] = '\0';
  for (char* p = reply; *p; p++) {
    if (*p == '\r' || *p == '\n') *p = ' ';
  }
  return true;
}

bool callFreeAi(const char* prompt, char* reply, size_t replyLen, char* err, size_t errLen) {
  reply[0] = '\0';
  err[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(err, "No WiFi", errLen - 1);
    err[errLen - 1] = '\0';
    return false;
  }

  // Prefer free Groq key when configured (fast + reliable).
  if (groqApiKey[0]) {
    if (callGroqAi(prompt, reply, replyLen, err, errLen)) return true;
  }

  // Zero-key fallback.
  char pollErr[64];
  if (callPollinationsAi(prompt, reply, replyLen, pollErr, sizeof(pollErr))) return true;

  if (groqApiKey[0]) {
    // Keep Groq error if key was set but both failed.
    if (!err[0]) {
      strncpy(err, pollErr, errLen - 1);
      err[errLen - 1] = '\0';
    }
  } else {
    snprintf(err, errLen, "%s — tap Key for free Groq", pollErr);
  }
  return false;
}

void aiWorkerTask(void* param) {
  (void)param;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    char promptCopy[PROMPT_BUF];
    if (aiMutex && xSemaphoreTake(aiMutex, portMAX_DELAY) == pdTRUE) {
      strncpy(promptCopy, aiPendingPrompt, sizeof(promptCopy) - 1);
      promptCopy[sizeof(promptCopy) - 1] = '\0';
      xSemaphoreGive(aiMutex);
    } else {
      strncpy(promptCopy, aiPendingPrompt, sizeof(promptCopy) - 1);
      promptCopy[sizeof(promptCopy) - 1] = '\0';
    }

    char reply[REPLY_BUF];
    char err[64];
    bool ok = callFreeAi(promptCopy, reply, sizeof(reply), err, sizeof(err));

    if (aiMutex && xSemaphoreTake(aiMutex, portMAX_DELAY) == pdTRUE) {
      if (ok) {
        strncpy(aiReplyText, reply, sizeof(aiReplyText) - 1);
        aiReplyText[sizeof(aiReplyText) - 1] = '\0';
        aiErrorText[0] = '\0';
      } else {
        aiReplyText[0] = '\0';
        strncpy(aiErrorText, err, sizeof(aiErrorText) - 1);
        aiErrorText[sizeof(aiErrorText) - 1] = '\0';
      }
      aiReplyReady = true;
      aiRequestPending = false;
      xSemaphoreGive(aiMutex);
    } else {
      if (ok) {
        strncpy(aiReplyText, reply, sizeof(aiReplyText) - 1);
        aiReplyText[sizeof(aiReplyText) - 1] = '\0';
        aiErrorText[0] = '\0';
      } else {
        aiReplyText[0] = '\0';
        strncpy(aiErrorText, err, sizeof(aiErrorText) - 1);
        aiErrorText[sizeof(aiErrorText) - 1] = '\0';
      }
      aiReplyReady = true;
      aiRequestPending = false;
    }
  }
}

void startAiTask() {
  if (!aiMutex) aiMutex = xSemaphoreCreateMutex();
  if (!aiTaskHandle) {
    xTaskCreatePinnedToCore(
      aiWorkerTask,
      "aiWorker",
      16384,
      nullptr,
      1,
      &aiTaskHandle,
      0
    );
  }
}

void requestAiReply() {
  if (aiBusy || aiRequestPending) return;
  if (WiFi.status() != WL_CONNECTED) {
    addMessage(false, "Connect WiFi first (tap WiFi).");
    setStatus("Offline");
    return;
  }
  buildAiPrompt(aiPendingPrompt, sizeof(aiPendingPrompt));
  aiBusy = true;
  aiRequestPending = true;
  aiReplyReady = false;
  setStatus("Thinking...");
  if (aiTaskHandle) xTaskNotifyGive(aiTaskHandle);
}

void applyAiReply() {
  if (!aiReplyReady) return;
  char reply[REPLY_BUF];
  char err[64];
  if (aiMutex && xSemaphoreTake(aiMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    strncpy(reply, aiReplyText, sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';
    strncpy(err, aiErrorText, sizeof(err) - 1);
    err[sizeof(err) - 1] = '\0';
    aiReplyReady = false;
    xSemaphoreGive(aiMutex);
  } else {
    strncpy(reply, aiReplyText, sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';
    strncpy(err, aiErrorText, sizeof(err) - 1);
    err[sizeof(err) - 1] = '\0';
    aiReplyReady = false;
  }
  aiBusy = false;
  if (reply[0]) {
    addMessage(false, reply);
    setStatus("Online — ask me anything");
  } else {
    char msg[96];
    snprintf(msg, sizeof(msg), "Sorry, AI failed (%s).", err[0] ? err : "error");
    addMessage(false, msg);
    setStatus("Tap to retry");
  }
}

void sendDraftMessage() {
  if (aiBusy) return;
  // trim
  size_t start = 0;
  while (draft[start] == ' ') start++;
  size_t end = strlen(draft);
  while (end > start && draft[end - 1] == ' ') end--;
  if (end <= start) return;
  char text[DRAFT_LEN];
  size_t n = end - start;
  if (n >= sizeof(text)) n = sizeof(text) - 1;
  memcpy(text, draft + start, n);
  text[n] = '\0';
  clearDraft();
  uiMode = UI_CHAT;
  addMessage(true, text);
  requestAiReply();
}

void drawHeader() {
  lcd.fillRoundRect(8, 4, SCR_W - 16, HEADER_H, 10, C(255, 255, 255));
  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(2);
  lcd.drawString("TC CHAT", 48, 10);
  drawBatteryMeter(BAT_ICON_X, BAT_ICON_Y);

  lcd.fillRoundRect(196, 8, 44, 22, 8, groqApiKey[0] ? C(0, 184, 148) : C(162, 155, 254));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1);
  lcd.drawString("Key", 208, 14);

  lcd.fillRoundRect(248, 8, 56, 22, 8, C(116, 185, 255));
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("WiFi", 260, 14);

  lcd.fillRect(8, HEADER_H + 2, SCR_W - 16, STATUS_H, C(245, 247, 255));
  lcd.setTextColor(aiBusy ? C(253, 150, 68) : C(100, 100, 120));
  lcd.setTextSize(1);
  lcd.drawString(statusLine, 14, HEADER_H + 6);
}

void drawMessageArea() {
  int y1 = (uiMode == UI_COMPOSE) ? MSG_AREA_Y1_KB : MSG_AREA_Y1_FULL;
  int areaH = y1 - MSG_AREA_Y0;
  lcd.fillRoundRect(8, MSG_AREA_Y0, SCR_W - 16, areaH, 10, C(255, 255, 255));

  if (msgCount == 0) {
    lcd.setTextColor(C(140, 140, 160));
    lcd.setTextSize(1);
    lcd.drawString("Tap the bar below to type.", 24, MSG_AREA_Y0 + 28);
    lcd.drawString("Free AI — Key optional (Groq).", 24, MSG_AREA_Y0 + 44);
    return;
  }

  int visible = (uiMode == UI_COMPOSE) ? 3 : 5;
  int bubbleH = (areaH - 8) / visible;
  int start = msgScroll;
  if (start < 0) start = 0;
  if (start > msgCount - 1) start = msgCount - 1;

  for (int i = 0; i < visible; i++) {
    int idx = start + i;
    if (idx >= msgCount) break;
    int y = MSG_AREA_Y0 + 4 + i * bubbleH;
    bool user = messages[idx].fromUser;
    int bx = user ? 70 : 14;
    int bw = SCR_W - 28 - 56;
    uint16_t bg = user ? C(108, 92, 231) : C(245, 247, 255);
    uint16_t fg = user ? TFT_WHITE : C(40, 40, 60);
    lcd.fillRoundRect(bx, y, bw, bubbleH - 4, 8, bg);
    lcd.setTextColor(user ? C(200, 210, 255) : C(0, 184, 148));
    lcd.setTextSize(1);
    lcd.drawString(user ? "You" : "AI", bx + 6, y + 3);
    wrapDrawText(messages[idx].text, bx + 6, y + 15, bw - 12, 2, fg);
  }

  // Scroll hints
  if (msgScroll > 0) {
    lcd.setTextColor(C(108, 92, 231));
    lcd.drawString("^", SCR_W - 22, MSG_AREA_Y0 + 6);
  }
  if (msgScroll + visible < msgCount) {
    lcd.setTextColor(C(108, 92, 231));
    lcd.drawString("v", SCR_W - 22, y1 - 14);
  }
}

void drawInputBar(bool composing) {
  int y = composing ? INPUT_Y_KB : INPUT_Y_FULL;
  lcd.fillRoundRect(8, y, 220, INPUT_H, 10, C(255, 255, 255));
  lcd.setTextColor(draft[0] ? C(40, 40, 60) : C(150, 150, 170));
  lcd.setTextSize(1);
  const char* shown = draft[0] ? draft : (composing ? "Type a message..." : "Tap to type...");
  // Show only the tail if draft is long
  char clip[40];
  if (strlen(shown) > 34) {
    snprintf(clip, sizeof(clip), "...%s", shown + strlen(shown) - 31);
    lcd.drawString(clip, 16, y + 10);
  } else {
    lcd.drawString(shown, 16, y + 10);
  }

  uint16_t sendCol = (aiBusy || !draft[0]) ? C(180, 185, 200) : C(0, 184, 148);
  lcd.fillRoundRect(236, y, 76, INPUT_H, 10, sendCol);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString(aiBusy ? "..." : "Send", 256, y + 10);
}

void drawComposeKeyboard() {
  const int keyW = 28;
  const int keyH = 22;
  int startY = KB_Y0;
  for (int row = 0; row < 4; row++) {
    const char* keys = kbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    for (int i = 0; i < len; i++) {
      char label[2] = { keys[i], '\0' };
      int x = startX + i * keyW;
      int y = startY + row * (keyH + 3);
      lcd.fillRoundRect(x, y, keyW - 2, keyH, 5, C(255, 255, 255));
      lcd.setTextColor(C(40, 40, 60));
      lcd.setTextSize(1);
      lcd.drawString(label, x + 9, y + 6);
    }
  }

  lcd.fillRoundRect(8, 220, 50, 18, 6, C(116, 185, 255));
  lcd.fillRoundRect(64, 220, 50, 18, 6, C(162, 155, 254));
  lcd.fillRoundRect(120, 220, 70, 18, 6, C(255, 255, 255));
  lcd.fillRoundRect(196, 220, 50, 18, 6, C(255, 118, 117));
  lcd.fillRoundRect(252, 220, 60, 18, 6, C(0, 184, 148));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1);
  lcd.drawString("Hide", 20, 225);
  lcd.drawString("Shift", 74, 225);
  lcd.setTextColor(C(40, 40, 60));
  lcd.drawString("Space", 140, 225);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Del", 210, 225);
  lcd.drawString("Send", 266, 225);
}

void drawChatScreen() {
  drawBackground();
  drawHeader();
  drawMessageArea();
  drawInputBar(uiMode == UI_COMPOSE);
  if (uiMode == UI_COMPOSE) drawComposeKeyboard();
  chatDirty = false;
}

void handleComposeTouch(int32_t tx, int32_t ty) {
  if (ty >= 8 && ty <= 30 && tx >= 248 && tx <= 304) {
    openWifiTool();
    return;
  }
  if (ty >= 8 && ty <= 30 && tx >= 196 && tx <= 240) {
    openApiKeyTool();
    return;
  }

  // Message scroll while composing
  if (ty >= MSG_AREA_Y0 && ty < MSG_AREA_Y1_KB) {
    int mid = (MSG_AREA_Y0 + MSG_AREA_Y1_KB) / 2;
    int visible = 3;
    if (ty < mid && msgScroll > 0) {
      msgScroll--;
      chatDirty = true;
    } else if (ty >= mid && msgScroll + visible < msgCount) {
      msgScroll++;
      chatDirty = true;
    }
    return;
  }

  // Send on input bar
  int iy = INPUT_Y_KB;
  if (ty >= iy && ty <= iy + INPUT_H && tx >= 236 && tx <= 312) {
    sendDraftMessage();
    return;
  }

  const int keyW = 28;
  const int keyH = 22;
  int startY = KB_Y0;
  for (int row = 0; row < 4; row++) {
    const char* keys = kbShift ? wifiKbRowsShift[row] : wifiKbRows[row];
    int len = strlen(keys);
    int startX = (SCR_W - len * keyW) / 2;
    int y = startY + row * (keyH + 3);
    if (ty < y || ty >= y + keyH) continue;
    for (int i = 0; i < len; i++) {
      int x = startX + i * keyW;
      if (tx >= x && tx < x + keyW - 2) {
        if (!aiBusy) appendDraftChar(keys[i]);
        return;
      }
    }
  }

  if (ty >= 220 && ty <= 238) {
    if (tx >= 8 && tx <= 58) { uiMode = UI_CHAT; chatDirty = true; return; }
    if (tx >= 64 && tx <= 114) { kbShift = !kbShift; chatDirty = true; return; }
    if (tx >= 120 && tx <= 190) { if (!aiBusy) appendDraftChar(' '); return; }
    if (tx >= 196 && tx <= 246) { if (!aiBusy) backspaceDraft(); return; }
    if (tx >= 252 && tx <= 312) { sendDraftMessage(); return; }
  }
}

void handleChatTouch(int32_t tx, int32_t ty) {
  if (ty >= 8 && ty <= 30 && tx >= 248 && tx <= 304) {
    openWifiTool();
    return;
  }
  if (ty >= 8 && ty <= 30 && tx >= 196 && tx <= 240) {
    openApiKeyTool();
    return;
  }

  // Scroll history
  if (ty >= MSG_AREA_Y0 && ty < MSG_AREA_Y1_FULL) {
    int mid = (MSG_AREA_Y0 + MSG_AREA_Y1_FULL) / 2;
    int visible = 5;
    if (ty < mid && msgScroll > 0) {
      msgScroll--;
      chatDirty = true;
    } else if (ty >= mid && msgScroll + visible < msgCount) {
      msgScroll++;
      chatDirty = true;
    }
    return;
  }

  int iy = INPUT_Y_FULL;
  if (ty >= iy && ty <= iy + INPUT_H) {
    if (tx >= 236 && tx <= 312) {
      sendDraftMessage();
      return;
    }
    uiMode = UI_COMPOSE;
    chatDirty = true;
  }
}

void handleTouch(int32_t tx, int32_t ty) {
  if (uiMode == UI_WIFI) { handleWifiTouch(tx, ty); return; }
  if (uiMode == UI_WIFI_PASS) { handleWifiPassTouch(tx, ty); return; }
  if (uiMode == UI_API_KEY) { handleApiKeyTouch(tx, ty); return; }
  if (uiMode == UI_COMPOSE) { handleComposeTouch(tx, ty); return; }
  handleChatTouch(tx, ty);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);

  lcd.init();
  lcd.setBrightness(255);
  lcd.setRotation(1);
  initTouchController();
  showSplashScreen();
  initBatteryMonitor();
  loadWifiCredentials();
  loadApiKey();
  startAiTask();

  splashStartMs = millis();
  wifiStartMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);

  if (!psramFound()) {
    showBootError("PSRAM not found", "Tools: OPI PSRAM ON");
    return;
  }

  addMessage(false, "Hi! Free on-device AI. Tap below to chat. Optional: Key for Groq.");
}

void loop() {
  switch (bootState) {
    case BOOT_SPLASH:
      if (millis() - splashStartMs >= SPLASH_MS) {
        chatDirty = true;
        if (WiFi.status() == WL_CONNECTED) {
          WiFi.setSleep(false);
          setStatus("Online — ask me anything");
          bootState = BOOT_DONE;
        } else {
          setStatus("Connecting WiFi...");
          bootState = BOOT_WIFI;
        }
      }
      return;

    case BOOT_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        setStatus("Online — ask me anything");
        bootState = BOOT_DONE;
        chatDirty = true;
      } else if (millis() - wifiStartMs > WIFI_TIMEOUT_MS) {
        setStatus("WiFi failed — tap WiFi");
        bootState = BOOT_DONE;
        openWifiTool();
      }
      break;

    default: break;
  }

  static bool touchDown = false;
  int32_t touchX, touchY;
  bool touching = (bootState == BOOT_DONE) && readTouchScreen(touchX, touchY);
  if (touching && !touchDown && (millis() - lastDebounceTime) > debounceDelay) {
    handleTouch(touchX, touchY);
    lastDebounceTime = millis();
  }
  touchDown = touching;

  updateBatteryMonitor();
  if (batCharging && millis() - lastBatAnimMs >= BAT_ANIM_MS) {
    batAnimFrame ^= 1;
    lastBatAnimMs = millis();
    batDirty = true;
  }
  if (batDirty) {
    if (uiMode == UI_WIFI || uiMode == UI_WIFI_PASS) wifiDirty = true;
    else chatDirty = true;
  }

  pollWifiScan();
  pollWifiConnect();
  applyAiReply();

  if (uiMode == UI_WIFI || uiMode == UI_WIFI_PASS) {
    if (wifiDirty) {
      if (uiMode == UI_WIFI_PASS) drawWifiPassScreen();
      else drawWifiScreen();
    }
  } else if (uiMode == UI_API_KEY) {
    if (chatDirty) drawApiKeyScreen();
  } else if (bootState == BOOT_DONE && chatDirty) {
    drawChatScreen();
  }
}
