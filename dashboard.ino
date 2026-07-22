/*
 * TC Radios — ES3C28P 2.8" ESP32-S3 (320x240 landscape)
 * Open as: Documents/Arduino/dashboard/dashboard.ino
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2S_AMP_EN    GPIO_NUM_1
#define I2S_MCLK      GPIO_NUM_4
#define I2S_BCLK      GPIO_NUM_5
#define I2S_DOUT      GPIO_NUM_8
#define I2S_DIN       GPIO_NUM_6
#define I2S_LRCK      GPIO_NUM_7
#define I2C_SDA       16
#define I2C_SCL       15
#define ES8311_ADDR   0x18

#define SCR_W         320
#define SCR_H         240
#define TOUCH_RST     18
#define TOUCH_INT     17
#define TOUCH_ADDR    0x38
#define MAX_STATIONS  50
#define WIFI_TIMEOUT_MS 30000
#define NAME_LEN      28
#define GENRE_LEN     22
#define URL_LEN       512
#define STREAM_SKIP_MS 12000
#define SPLASH_MS     5000
#define CACHE_PATH    "/stations.json"
#define MAX_WIFI_NETS 16
#define WIFI_SSID_LEN 33
#define WIFI_PASS_LEN 64
#define WIFI_BTN_X0   252
#define WIFI_BTN_X1   302
#define WIFI_BTN_Y0   10
#define WIFI_BTN_Y1   30
#define WIFI_LIST_Y0  52
#define WIFI_ROW_H    28
#define WIFI_VISIBLE  4
#define BAT_ADC_PIN   9
#define BAT_DIVIDER   2.0f
#define BAT_SAMPLES   8
#define BAT_UPDATE_MS 2000
#define BAT_ANIM_MS   450
#define BAT_ICON_X    14
#define BAT_ICON_Y    14
#define BAT_ICON_W    36
#define BAT_ICON_H    18
#define BAT_HISTORY   8
#define BAT_NO_BAT_V  4.30f

#define TILE_X        18
#define TILE_Y        34
#define TILE_SIZE     72

// Dock layout — volume row on top, transport row centered below with equal spacing
#define DOCK_Y        148
#define DOCK_H        86
#define VOL_Y         156
#define VOL_ROW_Y0    152
#define VOL_ROW_Y1    178
#define VOL_MINUS_X0  12
#define VOL_MINUS_X1  42
#define VOL_PLUS_X0   278
#define VOL_PLUS_X1   308
#define VOL_BAR_X0    48
#define VOL_BAR_X1    272
#define TRN_ROW_Y0    182
#define TRN_ROW_Y1    230
#define TRN_CY        204
#define BTN_PREV_CX   70
#define BTN_PLAY_CX   160
#define BTN_NEXT_CX   250
#define BTN_HIT_W     36
#define BTN_PREV_X0   (BTN_PREV_CX - BTN_HIT_W)
#define BTN_PREV_X1   (BTN_PREV_CX + BTN_HIT_W)
#define BTN_PLAY_X0   (BTN_PLAY_CX - BTN_HIT_W)
#define BTN_PLAY_X1   (BTN_PLAY_CX + BTN_HIT_W)
#define BTN_NEXT_X0   (BTN_NEXT_CX - BTN_HIT_W)
#define BTN_NEXT_X1   (BTN_NEXT_CX + BTN_HIT_W)

struct RadioStation {
  char name[NAME_LEN];
  char url[URL_LEN];
  char genre[GENRE_LEN];
};

RadioStation playlist[MAX_STATIONS];
int totalStations = 0;
int currentStationIdx = 0;

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
      cfg.invert = true;   // TFT_INVERSION_ON — required on ES3C28P
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
Audio audio;

char statusLine[40] = "Starting...";
char streamTitle[40] = "";
bool isPlaying = true;
bool audioReady = false;
bool streamPlaying = false;
bool userPaused = false;
bool bgDrawn = false;
bool stationDirty = true;
bool statusDirty = true;
bool dockDirty = true;
int volumeLevel = 12;
unsigned long streamConnectMs = 0;
unsigned long lastAutoSkipMs = 0;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

char wifiSsid[WIFI_SSID_LEN] = "simson";
char wifiPass[WIFI_PASS_LEN] = "jayatha10";
const char* jsonUrl = "https://raw.githubusercontent.com/simsonpeter/Tcradios/refs/heads/main/stations.json";

enum BootState { BOOT_SPLASH, BOOT_WIFI, BOOT_PLAYLIST, BOOT_AUDIO, BOOT_DONE };
enum UiMode { UI_RADIO, UI_WIFI, UI_WIFI_PASS };
BootState bootState = BOOT_SPLASH;
UiMode uiMode = UI_RADIO;
unsigned long wifiStartMs = 0;
unsigned long splashStartMs = 0;
bool playlistLoadStarted = false;
bool playlistRefreshPending = false;
bool fsReady = false;
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

SemaphoreHandle_t audioMutex = nullptr;
TaskHandle_t stationTaskHandle = nullptr;
volatile bool stationConnecting = false;
volatile int connectGen = 0;

uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); }

void drawBackground();
void drawStationPanel();
void drawPlayButtonOnly();
void drawVolumeOnly();
void setStatus(const char* msg);
bool parsePlaylistJson(const char* json);
bool loadPlaylistFromCache();
bool loadPlaylistFromGitHub();
void savePlaylistCache(const String& payload);
void showSplashScreen();
void drawStationTile();
char stationInitial(int idx);
void drawDock();
void updateStatusArea();
void connectCurrentStation();
void skipToNextStation();
void skipToPrevStation();
void initAudioHardware();
void showBootError(const char* line1, const char* line2);
bool initES8311Codec();
void setVolumeLevel(int v);
void handleTouch(int32_t tx, int32_t ty);
void initTouchController();
bool readTouchScreen(int32_t &sx, int32_t &sy);
void refreshPlaybackStatus();
void drawIconPrev(int cx, int cy, uint16_t col);
void drawIconNext(int cx, int cy, uint16_t col);
void drawIconPlay(int cx, int cy, uint16_t col);
void drawIconPause(int cx, int cy, uint16_t col);
bool extractJsonField(const char* json, const char* field, char* dest, size_t destLen);
void loadWifiCredentials();
void saveWifiCredentials();
void loadVolumeSetting();
void saveVolumeSetting();
void openWifiTool();
void closeWifiTool();
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
void initBatteryMonitor();
void updateBatteryMonitor();
float readBatteryVoltage();
int batteryVoltageToPercent(float volts);
void drawBatteryMeter(int x, int y);
void drawBatteryMeterOnly();
void drawBatteryBolt(int cx, int cy, uint16_t col);
bool audioLock(TickType_t ticks = portMAX_DELAY);
void audioUnlock();
void stationConnectTask(void* param);
void startStationConnectTask();

bool es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool initES8311Codec() {
  if (!es8311Write(0x00, 0x1F)) return false;
  delay(20);
  if (!es8311Write(0x00, 0x00)) return false;
  if (!es8311Write(0x00, 0x80)) return false;
  if (!es8311Write(0x01, 0x3F)) return false;
  if (!es8311Write(0x02, 0x00)) return false;
  if (!es8311Write(0x03, 0x10)) return false;
  if (!es8311Write(0x04, 0x10)) return false;
  if (!es8311Write(0x05, 0x00)) return false;
  if (!es8311Write(0x06, 0x03)) return false;
  if (!es8311Write(0x07, 0x00)) return false;
  if (!es8311Write(0x08, 0xFF)) return false;
  if (!es8311Write(0x09, 0x0C)) return false;
  if (!es8311Write(0x0A, 0x0C)) return false;
  if (!es8311Write(0x0D, 0x01)) return false;
  if (!es8311Write(0x0E, 0x02)) return false;
  if (!es8311Write(0x12, 0x00)) return false;
  if (!es8311Write(0x13, 0x10)) return false;
  if (!es8311Write(0x1C, 0x6A)) return false;
  if (!es8311Write(0x37, 0x08)) return false;
  if (!es8311Write(0x14, 0x1A)) return false;
  if (!es8311Write(0x31, 0x00)) return false;
  if (!es8311Write(0x32, 0xD8)) return false;
  return true;
}

void setVolumeLevel(int v) {
  if (v < 0) v = 0;
  if (v > 21) v = 21;
  bool changed = (v != volumeLevel);
  volumeLevel = v;
  // Codec volume always applies immediately, even while a stream is connecting.
  uint8_t reg = (v == 0) ? 0 : (uint8_t)((v * 255) / 21);
  es8311Write(0x32, reg);
  if (audioReady && audioLock(0)) {
    audio.setVolume(volumeLevel);
    audioUnlock();
  }
  if (changed) saveVolumeSetting();
}

bool extractJsonField(const char* json, const char* field, char* dest, size_t destLen) {
  char key[24];
  snprintf(key, sizeof(key), "\"%s\"", field);
  const char* pos = strstr(json, key);
  if (!pos) return false;
  pos += strlen(key);
  while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;
  if (*pos != '"') return false;
  pos++;
  size_t i = 0;
  while (*pos && *pos != '"' && i < destLen - 1) {
    if (*pos == '\\' && pos[1]) pos++;
    dest[i++] = *pos++;
  }
  dest[i] = '\0';
  return i > 0;
}

bool parsePlaylistJson(const char* json) {
  if (!json || !json[0]) return false;
  totalStations = 0;
  const char* cursor = json;
  while (totalStations < MAX_STATIONS) {
    const char* obj = strstr(cursor, "\"name\"");
    if (!obj) break;
    char name[NAME_LEN], url[URL_LEN], genre[GENRE_LEN];
    if (!extractJsonField(obj, "name", name, sizeof(name))) break;
    if (!extractJsonField(obj, "url", url, sizeof(url)) || strlen(url) == 0) {
      cursor = obj + 6;
      continue;
    }
    extractJsonField(obj, "genre", genre, sizeof(genre));
    strncpy(playlist[totalStations].name, name, NAME_LEN - 1);
    playlist[totalStations].name[NAME_LEN - 1] = '\0';
    strncpy(playlist[totalStations].url, url, URL_LEN - 1);
    playlist[totalStations].url[URL_LEN - 1] = '\0';
    strncpy(playlist[totalStations].genre, genre, GENRE_LEN - 1);
    playlist[totalStations].genre[GENRE_LEN - 1] = '\0';
    totalStations++;
    const char* nextUrl = strstr(obj, "\"url\"");
    cursor = nextUrl ? nextUrl + 5 : obj + 6;
  }
  return totalStations > 0;
}

bool loadPlaylistFromCache() {
  if (!fsReady || !LittleFS.exists(CACHE_PATH)) return false;
  File f = LittleFS.open(CACHE_PATH, "r");
  if (!f) return false;
  String payload = f.readString();
  f.close();
  return parsePlaylistJson(payload.c_str());
}

void savePlaylistCache(const String& payload) {
  if (!fsReady || payload.length() < 32) return;
  File f = LittleFS.open(CACHE_PATH, "w");
  if (!f) return;
  f.print(payload);
  f.close();
}

void showSplashScreen() {
  lcd.fillScreen(TFT_WHITE);
  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(3);
  const char* title = "TC RADIOS";
  int tw = lcd.textWidth(title);
  lcd.drawString(title, (SCR_W - tw) / 2, (SCR_H - 28) / 2);
}

char stationInitial(int idx) {
  if (idx < 0 || idx >= totalStations) return '?';
  const char* p = playlist[idx].name;
  while (*p && !isalnum((unsigned char)*p)) p++;
  char c = *p ? *p : '?';
  if (c >= 'a' && c <= 'z') c -= 32;
  return c;
}

void drawStationTile() {
  lcd.fillRoundRect(TILE_X - 3, TILE_Y - 3, TILE_SIZE + 6, TILE_SIZE + 6, 16, C(255, 255, 255));
  if (totalStations <= 0) return;
  uint16_t colors[] = { C(108, 92, 231), C(0, 184, 148), C(253, 121, 168), C(253, 150, 68) };
  uint16_t bg = colors[currentStationIdx % 4];
  lcd.fillRoundRect(TILE_X, TILE_Y, TILE_SIZE, TILE_SIZE, 14, bg);
  char letter[2] = { stationInitial(currentStationIdx), '\0' };
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(3);
  int tw = lcd.textWidth(letter);
  lcd.drawString(letter, TILE_X + (TILE_SIZE - tw) / 2, TILE_Y + 22);
}

void drawIconPrev(int cx, int cy, uint16_t col) {
  lcd.fillTriangle(cx - 14, cy, cx - 2, cy - 10, cx - 2, cy + 10, col);
  lcd.fillTriangle(cx - 4, cy, cx + 8, cy - 10, cx + 8, cy + 10, col);
}

void drawIconNext(int cx, int cy, uint16_t col) {
  lcd.fillTriangle(cx + 14, cy, cx + 2, cy - 10, cx + 2, cy + 10, col);
  lcd.fillTriangle(cx + 4, cy, cx - 8, cy - 10, cx - 8, cy + 10, col);
}

void drawIconPlay(int cx, int cy, uint16_t col) {
  (void)col;
  lcd.fillCircle(cx, cy, 22, C(255, 118, 117));
  lcd.fillTriangle(cx - 5, cy - 10, cx - 5, cy + 10, cx + 12, cy, TFT_WHITE);
}

void drawIconPause(int cx, int cy, uint16_t col) {
  (void)col;
  lcd.fillCircle(cx, cy, 22, C(255, 118, 117));
  lcd.fillRect(cx - 9, cy - 9, 6, 18, TFT_WHITE);
  lcd.fillRect(cx + 3, cy - 9, 6, 18, TFT_WHITE);
}

void drawDock() {
  lcd.fillRoundRect(8, DOCK_Y, SCR_W - 16, DOCK_H, 14, C(255, 255, 255));

  lcd.fillRoundRect(VOL_MINUS_X0, VOL_Y, 30, 26, 8, C(116, 185, 255));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.drawString("-", VOL_MINUS_X0 + 8, VOL_Y + 4);

  lcd.fillRoundRect(VOL_PLUS_X0, VOL_Y, 30, 26, 8, C(116, 185, 255));
  lcd.drawString("+", VOL_PLUS_X0 + 8, VOL_Y + 4);

  drawVolumeOnly();

  drawIconPrev(BTN_PREV_CX, TRN_CY, C(108, 92, 231));
  if (isPlaying) drawIconPause(BTN_PLAY_CX, TRN_CY, 0);
  else drawIconPlay(BTN_PLAY_CX, TRN_CY, 0);
  drawIconNext(BTN_NEXT_CX, TRN_CY, C(108, 92, 231));
  dockDirty = false;
}

void drawBackground() {
  if (bgDrawn) return;
  for (int y = 0; y < SCR_H; y++) {
    uint8_t t = (y * 255) / (SCR_H - 1);
    uint8_t r = 108 - (t * 40 / 255);
    uint8_t g = 92 + (t * 40 / 255);
    uint8_t b = 231 - (t * 4 / 255);
    lcd.drawFastHLine(0, y, SCR_W, C(r, g, b));
  }
  bgDrawn = true;
}

void drawStationPanel() {
  lcd.fillRoundRect(10, 8, SCR_W - 20, 118, 16, C(255, 255, 255));
  lcd.drawRoundRect(10, 8, SCR_W - 20, 118, 16, C(200, 210, 255));

  drawStationTile();

  lcd.setTextColor(C(108, 92, 231));
  lcd.setTextSize(2);
  lcd.drawString("TC RADIOS", 98, 12);

  drawBatteryMeter(BAT_ICON_X, BAT_ICON_Y);

  lcd.fillRoundRect(WIFI_BTN_X0, WIFI_BTN_Y0, WIFI_BTN_X1 - WIFI_BTN_X0, WIFI_BTN_Y1 - WIFI_BTN_Y0, 8, C(116, 185, 255));
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1);
  lcd.drawString("WiFi", WIFI_BTN_X0 + 12, WIFI_BTN_Y0 + 6);

  if (totalStations > 0) {
    lcd.fillRect(98, 32, 210, 52, C(255, 255, 255));
    lcd.setTextColor(C(40, 40, 60));
    lcd.setTextSize(2);
    lcd.drawString(playlist[currentStationIdx].name, 100, 36);
    lcd.setTextColor(C(253, 121, 168));
    lcd.setTextSize(1);
    lcd.drawString(playlist[currentStationIdx].genre, 100, 64);
    char idx[12];
    snprintf(idx, sizeof(idx), "%d/%d", currentStationIdx + 1, totalStations);
    lcd.setTextColor(C(120, 120, 140));
    lcd.drawString(idx, 100, 78);
  }

  stationDirty = false;
}

void drawLiveBadgeOnly() {
  // LIVE indicator lives in the status strip now; keep for play/pause refresh hooks
}

void drawPlayButtonOnly() {
  lcd.fillRect(BTN_PLAY_X0, TRN_ROW_Y0, BTN_PLAY_X1 - BTN_PLAY_X0, TRN_ROW_Y1 - TRN_ROW_Y0, C(255, 255, 255));
  if (isPlaying) drawIconPause(BTN_PLAY_CX, TRN_CY, 0);
  else drawIconPlay(BTN_PLAY_CX, TRN_CY, 0);
}

void drawVolumeOnly() {
  int bx = 52, bw = 216, bh = 10;
  lcd.fillRect(VOL_BAR_X0 - 2, VOL_Y + 5, VOL_BAR_X1 - VOL_BAR_X0 + 4, 16, C(255, 255, 255));
  lcd.fillRoundRect(bx, VOL_Y + 7, bw, bh, 5, C(220, 225, 240));
  int fill = (volumeLevel * bw) / 21;
  if (fill > 0) lcd.fillRoundRect(bx, VOL_Y + 7, fill, bh, 5, C(108, 92, 231));
}

void updateStatusArea() {
  lcd.fillRoundRect(18, 88, SCR_W - 36, 28, 10, C(245, 247, 255));
  lcd.setTextSize(1);
  if (streamPlaying && isPlaying) {
    lcd.setTextColor(C(0, 184, 148));
    lcd.drawString("ON AIR", 26, 98);
    lcd.setTextColor(C(60, 60, 80));
    lcd.drawString(streamTitle[0] ? streamTitle : statusLine, 72, 98);
  } else if (userPaused) {
    lcd.setTextColor(C(116, 185, 255));
    lcd.drawString("PAUSED", 26, 98);
    lcd.setTextColor(C(60, 60, 80));
    lcd.drawString(streamTitle[0] ? streamTitle : statusLine, 72, 98);
  } else {
    lcd.setTextColor(C(253, 150, 68));
    lcd.drawString("...", 26, 98);
    lcd.setTextColor(C(100, 100, 120));
    lcd.drawString(statusLine, 72, 98);
  }
  statusDirty = false;
}

void setStatus(const char* msg) {
  strncpy(statusLine, msg, sizeof(statusLine) - 1);
  statusLine[sizeof(statusLine) - 1] = '\0';
  statusDirty = true;
}

void refreshPlaybackStatus() {
  if (!audioReady) return;
  if (audio.isRunning() && !streamPlaying) {
    streamPlaying = true;
    userPaused = false;
    if (!streamTitle[0]) setStatus("Now Playing");
    statusDirty = true;
    drawLiveBadgeOnly();
  }
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

  // Portrait raw -> landscape (rotation 1). Mirror X to match on-screen left/right.
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

  lcd.fillRect(x - 1, y - 1, BAT_ICON_W + 2, BAT_ICON_H + 2, C(255, 255, 255));

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
  if (fillW > 0) {
    lcd.fillRoundRect(x + 2, y + 5, fillW, bodyH - 4, 2, fillCol);
  }

  if (batCharging && batAnimFrame) {
    drawBatteryBolt(x + 12, y + 9, C(253, 221, 68));
  }

  batDisplayPct = batPercent;
  batDirty = false;
}

void drawBatteryMeterOnly() {
  drawBatteryMeter(BAT_ICON_X, BAT_ICON_Y);
}

void handleTouch(int32_t tx, int32_t ty) {
  if (uiMode == UI_WIFI) {
    handleWifiTouch(tx, ty);
    return;
  }
  if (uiMode == UI_WIFI_PASS) {
    handleWifiPassTouch(tx, ty);
    return;
  }

  if (ty >= WIFI_BTN_Y0 && ty <= WIFI_BTN_Y1 && tx >= WIFI_BTN_X0 && tx <= WIFI_BTN_X1) {
    openWifiTool();
    return;
  }

  if (ty >= VOL_ROW_Y0 && ty <= VOL_ROW_Y1) {
    if (tx >= VOL_MINUS_X0 && tx <= VOL_MINUS_X1) {
      setVolumeLevel(volumeLevel - 1);
      drawVolumeOnly();
      return;
    }
    if (tx >= VOL_PLUS_X0 && tx <= VOL_PLUS_X1) {
      setVolumeLevel(volumeLevel + 1);
      drawVolumeOnly();
      return;
    }
    if (tx >= VOL_BAR_X0 && tx <= VOL_BAR_X1) {
      setVolumeLevel(((tx - VOL_BAR_X0) * 21) / (VOL_BAR_X1 - VOL_BAR_X0));
      drawVolumeOnly();
    }
    return;
  }

  if (ty >= TRN_ROW_Y0 && ty <= TRN_ROW_Y1) {
    if (tx >= BTN_PREV_X0 && tx <= BTN_PREV_X1) {
      if (totalStations > 0) skipToPrevStation();
      return;
    }
    if (tx >= BTN_PLAY_X0 && tx <= BTN_PLAY_X1) {
      if (audioReady && !stationConnecting && audioLock(0)) {
        isPlaying = !isPlaying;
        userPaused = !isPlaying;
        audio.pauseResume();
        audioUnlock();
        drawPlayButtonOnly();
        statusDirty = true;
        drawLiveBadgeOnly();
      }
      return;
    }
    if (tx >= BTN_NEXT_X0 && tx <= BTN_NEXT_X1) {
      if (totalStations > 0) skipToNextStation();
    }
  }
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

void loadVolumeSetting() {
  wifiPrefs.begin("audio", true);
  int v = wifiPrefs.getInt("volume", volumeLevel);
  wifiPrefs.end();
  if (v < 0) v = 0;
  if (v > 21) v = 21;
  volumeLevel = v;
}

void saveVolumeSetting() {
  wifiPrefs.begin("audio", false);
  wifiPrefs.putInt("volume", volumeLevel);
  wifiPrefs.end();
}

void openWifiTool() {
  if (audioReady) {
    connectGen++;  // cancel in-flight station connect
    if (audioLock(pdMS_TO_TICKS(50))) {
      audio.stopSong();
      audioUnlock();
    }
    streamPlaying = false;
    isPlaying = false;
  }
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
  uiMode = UI_RADIO;
  bgDrawn = false;
  stationDirty = true;
  statusDirty = true;
  dockDirty = true;
  wifiDirty = false;
  wifiScanning = false;
  wifiConnecting = false;
  if (WiFi.status() != WL_CONNECTED) return;

  if (totalStations == 0) {
    setStatus("Loading stations...");
    loadPlaylistFromGitHub();
    stationDirty = true;
  }
  if (!audioReady) initAudioHardware();
  if (audioReady && totalStations > 0) {
    isPlaying = true;
    userPaused = false;
    connectCurrentStation();
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
  if (wifiConnecting) {
    lcd.drawString("Connecting...", 20, 46);
  } else if (wifiScanning) {
    lcd.drawString("Scanning...", 20, 46);
  } else if (WiFi.status() == WL_CONNECTED) {
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
      char line[40];
      snprintf(line, sizeof(line), "%s", wifiFoundSsid[idx]);
      lcd.drawString(line, 26, y + 8);
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

  if (ty >= 16 && ty <= 40 && tx >= 18 && tx <= 74) {
    closeWifiTool();
    return;
  }
  if (ty >= 16 && ty <= 40 && tx >= 246 && tx <= 302) {
    startWifiScan();
    return;
  }

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
    if (tx >= 135 && tx <= 185) {
      connectSelectedWifi();
      return;
    }
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

  if (ty >= 28 && ty <= 54 && tx >= 260 && tx <= 304) {
    backspaceWifiPass();
    return;
  }

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
    if (tx >= 14 && tx <= 70) {
      uiMode = UI_WIFI;
      wifiDirty = true;
      return;
    }
    if (tx >= 80 && tx <= 136) {
      wifiKbShift = !wifiKbShift;
      wifiDirty = true;
      return;
    }
    if (tx >= 146 && tx <= 216) {
      appendWifiPassChar(' ');
      return;
    }
    if (tx >= 226 && tx <= 304) {
      joinWifiWithPassword();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(I2S_AMP_EN, OUTPUT);
  digitalWrite(I2S_AMP_EN, HIGH);
  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);

  lcd.init();
  lcd.setBrightness(255);
  lcd.setRotation(1);
  initTouchController();
  showSplashScreen();
  initBatteryMonitor();

  fsReady = LittleFS.begin(true);
  if (fsReady) loadPlaylistFromCache();

  loadWifiCredentials();
  loadVolumeSetting();
  splashStartMs = millis();
  wifiStartMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);

  if (!psramFound()) {
    showBootError("PSRAM not found", "Tools: OPI PSRAM ON");
    return;
  }
}

void loop() {
  switch (bootState) {
    case BOOT_SPLASH:
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        if (totalStations == 0 && !playlistLoadStarted) {
          playlistLoadStarted = true;
          loadPlaylistFromGitHub();
        } else if (totalStations > 0) {
          playlistRefreshPending = true;
        }
      }
      if (millis() - splashStartMs >= SPLASH_MS) {
        bgDrawn = false;
        stationDirty = true;
        statusDirty = true;
        dockDirty = true;
        if (totalStations > 0) {
          setStatus(WiFi.status() == WL_CONNECTED ? "Ready" : "Waiting WiFi...");
          bootState = (WiFi.status() == WL_CONNECTED) ? BOOT_AUDIO : BOOT_WIFI;
        } else {
          setStatus("Loading stations...");
          bootState = BOOT_WIFI;
        }
      }
      return;

    case BOOT_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        if (totalStations == 0) {
          bootState = BOOT_PLAYLIST;
        } else {
          bootState = BOOT_AUDIO;
        }
      } else if (millis() - wifiStartMs > WIFI_TIMEOUT_MS) {
        setStatus(totalStations > 0 ? "Offline (cached)" : "WiFi failed");
        if (totalStations > 0) {
          bootState = BOOT_AUDIO;
        } else {
          bootState = BOOT_DONE;
          openWifiTool();
        }
      }
      break;

    case BOOT_PLAYLIST:
      if (!playlistLoadStarted) {
        playlistLoadStarted = true;
        loadPlaylistFromGitHub();
        setStatus(totalStations > 0 ? "Ready" : "No stations");
        stationDirty = true;
        statusDirty = true;
        dockDirty = true;
        bootState = BOOT_AUDIO;
      }
      break;

    case BOOT_AUDIO:
      initAudioHardware();
      if (totalStations > 0) connectCurrentStation();
      bootState = BOOT_DONE;
      break;

    default: break;
  }

  // Touch + UI first so controls stay live while streams connect in the background.
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
    if (uiMode == UI_RADIO && bootState == BOOT_DONE && !stationDirty) {
      drawBatteryMeterOnly();
    } else if (uiMode != UI_RADIO) {
      wifiDirty = true;
    } else if (bootState == BOOT_DONE) {
      stationDirty = true;
    }
  }

  pollWifiScan();
  pollWifiConnect();

  if (bootState == BOOT_DONE && playlistRefreshPending && WiFi.status() == WL_CONNECTED) {
    playlistRefreshPending = false;
    int prevIdx = currentStationIdx;
    if (loadPlaylistFromGitHub()) {
      if (currentStationIdx >= totalStations) currentStationIdx = 0;
      stationDirty = true;
      if (prevIdx != currentStationIdx && audioReady) connectCurrentStation();
    }
  }

  if (uiMode != UI_RADIO) {
    if (wifiDirty) {
      bgDrawn = false;
      if (uiMode == UI_WIFI_PASS) drawWifiPassScreen();
      else drawWifiScreen();
    }
  } else {
    if (bootState == BOOT_DONE && audioReady && !userPaused && !stationConnecting &&
        !streamPlaying && !audio.isRunning() &&
        streamConnectMs > 0 && (millis() - streamConnectMs > STREAM_SKIP_MS)) {
      setStatus("Skipping...");
      skipToNextStation();
    }

    if (stationDirty) {
      drawBackground();
      drawStationPanel();
      statusDirty = true;
    }
    if (statusDirty) updateStatusArea();
    if (dockDirty) drawDock();
  }

  if (audioReady && audioLock(0)) {
    audio.loop();
    refreshPlaybackStatus();
    audioUnlock();
  }
}

void showBootError(const char* line1, const char* line2) {
  lcd.fillRect(20, 90, 280, 60, TFT_BLACK);
  lcd.setTextColor(TFT_RED);
  lcd.setTextSize(2);
  lcd.drawString(line1, 30, 95);
  lcd.setTextSize(1);
  lcd.drawString(line2, 30, 120);
}

void initAudioHardware() {
  if (audioReady) return;
  pinMode(I2S_AMP_EN, OUTPUT);
  digitalWrite(I2S_AMP_EN, LOW);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  delay(30);
  if (!initES8311Codec()) { setStatus("Codec error"); return; }
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volumeLevel);
  uint8_t reg = (volumeLevel == 0) ? 0 : (uint8_t)((volumeLevel * 255) / 21);
  es8311Write(0x32, reg);
  audio.setConnectionTimeout(2500, 3000);
  audioReady = true;
  startStationConnectTask();
}

bool audioLock(TickType_t ticks) {
  if (!audioMutex) return true;
  return xSemaphoreTake(audioMutex, ticks) == pdTRUE;
}

void audioUnlock() {
  if (audioMutex) xSemaphoreGive(audioMutex);
}

void stationConnectTask(void* param) {
  (void)param;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!audioReady || totalStations <= 0) continue;

    while (true) {
      int gen = connectGen;
      int idx = currentStationIdx;
      if (idx < 0 || idx >= totalStations) break;

      char urlCopy[URL_LEN];
      strncpy(urlCopy, playlist[idx].url, URL_LEN - 1);
      urlCopy[URL_LEN - 1] = '\0';

      stationConnecting = true;
      if (audioLock(portMAX_DELAY)) {
        audio.stopSong();
        if (gen == connectGen && idx == currentStationIdx) {
          audio.connecttohost(urlCopy);
        }
        audioUnlock();
      }
      stationConnecting = false;

      // User changed station while we were connecting — try the new one.
      if (gen != connectGen || idx != currentStationIdx) {
        streamConnectMs = millis();
        setStatus("Connecting...");
        stationDirty = true;
        statusDirty = true;
        continue;
      }
      break;
    }
  }
}

void startStationConnectTask() {
  if (!audioMutex) audioMutex = xSemaphoreCreateMutex();
  if (!stationTaskHandle) {
    xTaskCreatePinnedToCore(
      stationConnectTask,
      "stationConnect",
      8192,
      nullptr,
      1,
      &stationTaskHandle,
      0
    );
  }
}

void skipToNextStation() {
  if (totalStations <= 0) return;
  currentStationIdx = (currentStationIdx + 1) % totalStations;
  connectCurrentStation();
}

void skipToPrevStation() {
  if (totalStations <= 0) return;
  currentStationIdx = (currentStationIdx - 1 + totalStations) % totalStations;
  connectCurrentStation();
}

bool loadPlaylistFromGitHub() {
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(jsonUrl);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }

  String payload = http.getString();
  http.end();
  if (!parsePlaylistJson(payload.c_str())) return false;
  savePlaylistCache(payload);
  return true;
}

void connectCurrentStation() {
  if (!audioReady || totalStations <= 0) return;
  userPaused = false;
  isPlaying = true;
  streamPlaying = false;
  streamTitle[0] = '\0';
  streamConnectMs = millis();
  setStatus("Connecting...");
  stationDirty = true;
  statusDirty = true;
  dockDirty = true;
  connectGen++;
  if (stationTaskHandle) xTaskNotifyGive(stationTaskHandle);
}

void audio_showstreamtitle(const char *info) {
  if (!info || !info[0]) return;
  strncpy(streamTitle, info, sizeof(streamTitle) - 1);
  streamTitle[sizeof(streamTitle) - 1] = '\0';
  streamPlaying = true;
  statusDirty = true;
}

void audio_info(const char *info) {
  Serial.printf("Audio: %s\n", info);
  if (!info) return;
  String msg(info);
  if (msg.indexOf("bitrate") >= 0 || msg.indexOf("PLAY") >= 0 ||
      msg.indexOf("Connected") >= 0 || msg.indexOf("MP3") >= 0 || msg.indexOf("AAC") >= 0) {
    streamPlaying = true;
    setStatus("Now Playing");
  } else if (msg.indexOf("failed") >= 0 || msg.indexOf("404") >= 0 || msg.indexOf("timeout") >= 0) {
    setStatus("Stream error");
    if (millis() - lastAutoSkipMs > 3000) {
      lastAutoSkipMs = millis();
      skipToNextStation();
    }
  }
}
