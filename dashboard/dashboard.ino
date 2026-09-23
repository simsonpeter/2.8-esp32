/*
 * Internet radio — ES3C28P 2.8" ESP32-S3 (320x240)
 * Wiki: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
 *
 * Features: station list, Wi-Fi setup, battery meter, NTP clock,
 *           sleep timer, idle dim, LittleFS playlist cache.
 *
 * Arduino IDE: open dashboard/dashboard.ino
 * Board: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, Partition Scheme Custom
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <Wire.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
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
#define TOUCH_RST     18
#define TOUCH_INT     17
#define TOUCH_ADDR    0x38
#define BAT_ADC_PIN   9

#define SCR_W         320
#define SCR_H         240
#define MAX_STATIONS  40
#define MAX_WIFI_NETWORKS 12
#define NAME_LEN      28
#define URL_LEN       512
#define WIFI_TIMEOUT_MS 20000
#define STREAM_SKIP_MS  12000
#define IDLE_DIM_MS     45000
#define BRIGHT_FULL     255
#define BRIGHT_DIM      48
#define CACHE_PATH      "/stations.json"

#define C(r, g, b) lgfx::color565(r, g, b)
#define COL_BG     C(16, 20, 32)
#define COL_CARD   C(28, 34, 52)
#define COL_ACCENT C(88, 166, 255)
#define COL_PLAY   C(255, 107, 107)
#define COL_MUTED  C(140, 148, 168)
#define COL_OK     C(46, 204, 113)
#define COL_WARN   C(255, 177, 66)
#define COL_DANGER C(231, 76, 60)

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
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
      cfg.invert = true;  // required on ES3C28P
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

struct RadioStation {
  char name[NAME_LEN];
  char url[URL_LEN];
};

static LGFX lcd;
static Audio audio;
static Preferences prefs;

static RadioStation playlist[MAX_STATIONS];
static int totalStations = 0;
static int currentStationIdx = 0;
static int volumeLevel = 12;
static int listOffset = 0;

static char wifiSsid[33] = "simson";
static char wifiPass[64] = "jayatha10";
static const char* jsonUrl = "https://raw.githubusercontent.com/simsonpeter/Tcradios/refs/heads/main/stations.json";

static char scannedSsids[MAX_WIFI_NETWORKS][33];
static int scannedCount = 0;
static int wifiListOffset = 0;

static char statusLine[40] = "Starting...";
static char streamTitle[64] = "";
static char clockText[8] = "--:--";
static bool isPlaying = true;
static bool audioReady = false;
static bool streamPlaying = false;
static bool userPaused = false;
static bool fsReady = false;
static bool clockSynced = false;
static bool screenDimmed = false;
static volatile bool stationConnecting = false;
static volatile int connectGen = 0;
static unsigned long streamConnectMs = 0;
static unsigned long lastAutoSkipMs = 0;
static unsigned long lastDebounceTime = 0;
static unsigned long lastTouchMs = 0;
static unsigned long lastBatteryMs = 0;
static unsigned long lastClockMs = 0;
static unsigned long sleepUntilMs = 0;
static int sleepMinutes = 0;
static float batteryVoltage = 0.0f;
static int batteryPercent = 0;

static SemaphoreHandle_t audioMutex = nullptr;
static TaskHandle_t stationTaskHandle = nullptr;

static bool stationDirty = true;
static bool statusDirty = true;
static bool dockDirty = true;
static bool headerDirty = true;

static bool es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool initES8311Codec() {
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

static bool audioLock(TickType_t ticks = portMAX_DELAY) {
  if (!audioMutex) return true;
  return xSemaphoreTake(audioMutex, ticks) == pdTRUE;
}

static void audioUnlock() {
  if (audioMutex) xSemaphoreGive(audioMutex);
}

static void setStatus(const char* msg) {
  strncpy(statusLine, msg, sizeof(statusLine) - 1);
  statusLine[sizeof(statusLine) - 1] = '\0';
  statusDirty = true;
}

static void bumpActivity() {
  lastTouchMs = millis();
  if (screenDimmed) {
    lcd.setBrightness(BRIGHT_FULL);
    screenDimmed = false;
  }
}

static void addStation(const char* name, const char* url) {
  if (totalStations >= MAX_STATIONS || !url || !url[0]) return;
  strncpy(playlist[totalStations].name, name, NAME_LEN - 1);
  playlist[totalStations].name[NAME_LEN - 1] = '\0';
  strncpy(playlist[totalStations].url, url, URL_LEN - 1);
  playlist[totalStations].url[URL_LEN - 1] = '\0';
  totalStations++;
}

static void loadBuiltinStations() {
  totalStations = 0;
  addStation("SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3");
  addStation("Radio Paradise", "http://stream.radioparadise.com/mp3-128");
  addStation("FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3");
  addStation("NPR News", "http://npr-ice.streamguys1.com/live.mp3");
  addStation("BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service");
  addStation("SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3");
  addStation("SomaFM Folk Forward", "http://ice1.somafm.com/folkfwd-128-mp3");
  addStation("Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128");
}

static bool extractJsonFieldBounded(const char* start, const char* end, const char* field,
                                    char* dest, size_t destLen) {
  if (!start || !end || start >= end) return false;
  char key[24];
  snprintf(key, sizeof(key), "\"%s\"", field);
  size_t span = (size_t)(end - start);
  const char* pos = nullptr;
  for (size_t i = 0; i + strlen(key) <= span; i++) {
    if (strncmp(start + i, key, strlen(key)) == 0) {
      pos = start + i;
      break;
    }
  }
  if (!pos) return false;
  pos += strlen(key);
  while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
  if (pos >= end || *pos != '"') return false;
  pos++;
  size_t i = 0;
  while (pos < end && *pos && *pos != '"' && i < destLen - 1) {
    if (*pos == '\\' && (pos + 1) < end && pos[1]) pos++;
    dest[i++] = *pos++;
  }
  dest[i] = '\0';
  return i > 0;
}

static bool parsePlaylistJson(const char* json) {
  if (!json || !json[0]) return false;
  totalStations = 0;
  const char* cursor = json;
  while (totalStations < MAX_STATIONS) {
    const char* objStart = strchr(cursor, '{');
    if (!objStart) break;
    const char* objEnd = strchr(objStart, '}');
    if (!objEnd) break;
    objEnd++;  // exclusive end past '}'
    char name[NAME_LEN], url[URL_LEN];
    name[0] = url[0] = '\0';
    if (extractJsonFieldBounded(objStart, objEnd, "name", name, sizeof(name)) &&
        extractJsonFieldBounded(objStart, objEnd, "url", url, sizeof(url)) && url[0]) {
      addStation(name, url);
    }
    cursor = objEnd;
  }
  return totalStations > 0;
}

static bool loadPlaylistFromCache() {
  if (!fsReady || !LittleFS.exists(CACHE_PATH)) return false;
  File f = LittleFS.open(CACHE_PATH, "r");
  if (!f) return false;
  String payload = f.readString();
  f.close();
  return parsePlaylistJson(payload.c_str());
}

static void savePlaylistCache(const String& payload) {
  if (!fsReady || payload.length() == 0) return;
  File f = LittleFS.open(CACHE_PATH, "w");
  if (!f) return;
  f.print(payload);
  f.close();
}

static bool loadPlaylistFromGitHub() {
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(jsonUrl);
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  if (!parsePlaylistJson(payload.c_str())) return false;
  savePlaylistCache(payload);
  return true;
}

static void loadSettings() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", wifiSsid);
  String p = prefs.getString("pass", wifiPass);
  prefs.end();
  strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  strncpy(wifiPass, p.c_str(), sizeof(wifiPass) - 1);
  wifiPass[sizeof(wifiPass) - 1] = '\0';

  prefs.begin("audio", true);
  volumeLevel = prefs.getInt("volume", 12);
  currentStationIdx = prefs.getInt("station", 0);
  prefs.end();
  if (volumeLevel < 0) volumeLevel = 0;
  if (volumeLevel > 21) volumeLevel = 21;
}

static void saveWifiCredentials(const char* ssid, const char* password) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
  strncpy(wifiSsid, ssid, sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  strncpy(wifiPass, password, sizeof(wifiPass) - 1);
  wifiPass[sizeof(wifiPass) - 1] = '\0';
}

static void saveVolume() {
  prefs.begin("audio", false);
  prefs.putInt("volume", volumeLevel);
  prefs.end();
}

static void saveStation() {
  prefs.begin("audio", false);
  prefs.putInt("station", currentStationIdx);
  prefs.end();
}

static void setVolumeLevel(int v) {
  if (v < 0) v = 0;
  if (v > 21) v = 21;
  bool changed = (v != volumeLevel);
  volumeLevel = v;
  uint8_t reg = (v == 0) ? 0 : (uint8_t)((v * 255) / 21);
  es8311Write(0x32, reg);
  if (audioReady && audioLock(0)) {
    audio.setVolume(volumeLevel);
    audioUnlock();
  }
  if (changed) saveVolume();
  dockDirty = true;
}

static void updateBatteryReading() {
  batteryVoltage = analogReadMilliVolts(BAT_ADC_PIN) * 2.0f / 1000.0f;
  batteryPercent = (int)((batteryVoltage - 3.3f) * 100.0f / (4.2f - 3.3f));
  if (batteryPercent < 0) batteryPercent = 0;
  if (batteryPercent > 100) batteryPercent = 100;
  headerDirty = true;
}

static void updateClockText() {
  if (!clockSynced) {
    strncpy(clockText, "--:--", sizeof(clockText));
    headerDirty = true;
    return;
  }
  time_t now = time(nullptr);
  struct tm tmInfo;
  localtime_r(&now, &tmInfo);
  snprintf(clockText, sizeof(clockText), "%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min);
  headerDirty = true;
}

static void syncNtpClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      clockSynced = true;
      updateClockText();
      return;
    }
    delay(200);
  }
}

static void drawMessage(const char* line1, const char* line2 = nullptr) {
  lcd.fillScreen(COL_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString(line1, SCR_W / 2, SCR_H / 2 - (line2 ? 14 : 0));
  if (line2) {
    lcd.setFont(&fonts::DejaVu12);
    lcd.setTextColor(COL_MUTED);
    lcd.drawString(line2, SCR_W / 2, SCR_H / 2 + 16);
  }
}

static void drawHeader() {
  lcd.fillRect(0, 0, SCR_W, 22, COL_BG);
  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(COL_MUTED);
  lcd.drawString(clockText, 10, 11);

  if (sleepMinutes > 0) {
    char sleepLabel[12];
    unsigned long remain = 0;
    if (sleepUntilMs > millis()) remain = (sleepUntilMs - millis() + 59999UL) / 60000UL;
    snprintf(sleepLabel, sizeof(sleepLabel), "Zzz %lum", remain);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(COL_WARN);
    lcd.drawString(sleepLabel, SCR_W / 2, 11);
  }

  uint16_t fillColor = batteryPercent > 20 ? COL_OK : COL_DANGER;
  int barW = (batteryPercent * 28) / 100;
  lcd.drawRect(SCR_W - 48, 6, 32, 12, TFT_WHITE);
  lcd.fillRect(SCR_W - 16, 9, 3, 6, TFT_WHITE);
  lcd.fillRect(SCR_W - 46, 8, 28, 8, COL_CARD);
  if (barW > 0) lcd.fillRect(SCR_W - 46, 8, barW, 8, fillColor);
  headerDirty = false;
}

static void drawStationCard() {
  lcd.fillRoundRect(12, 26, SCR_W - 24, 78, 14, COL_CARD);

  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(COL_ACCENT);
  lcd.drawString("Internet Radio", 24, 34);

  char idx[12];
  snprintf(idx, sizeof(idx), "%d / %d", totalStations ? currentStationIdx + 1 : 0, totalStations);
  lcd.setTextDatum(textdatum_t::top_right);
  lcd.setTextColor(COL_MUTED);
  lcd.drawString(idx, SCR_W - 24, 34);

  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(TFT_WHITE);
  const char* name = totalStations > 0 ? playlist[currentStationIdx].name : "No stations";
  lcd.drawString(name, 24, 56);

  stationDirty = false;
  statusDirty = true;
}

static void drawStatus() {
  lcd.fillRect(24, 82, SCR_W - 48, 18, COL_CARD);
  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::top_left);

  const char* badge;
  uint16_t badgeCol;
  if (userPaused) {
    badge = "PAUSED";
    badgeCol = COL_ACCENT;
  } else if (streamPlaying && isPlaying) {
    badge = "ON AIR";
    badgeCol = COL_OK;
  } else {
    badge = "...";
    badgeCol = COL_WARN;
  }

  lcd.setTextColor(badgeCol);
  lcd.drawString(badge, 24, 84);
  lcd.setTextColor(COL_MUTED);
  const char* detail = streamTitle[0] ? streamTitle : statusLine;
  char clipped[36];
  strncpy(clipped, detail, sizeof(clipped) - 1);
  clipped[sizeof(clipped) - 1] = '\0';
  lcd.drawString(clipped, 92, 84);
  statusDirty = false;
}

static void drawVolume() {
  const int trackX = 56;
  const int trackY = 118;
  const int trackW = 168;
  const int trackH = 8;

  lcd.fillRect(12, 108, SCR_W - 24, 28, COL_BG);
  lcd.fillCircle(34, trackY + 4, 12, COL_CARD);
  lcd.fillRoundRect(28, trackY + 2, 12, 4, 2, TFT_WHITE);
  lcd.fillCircle(SCR_W - 34, trackY + 4, 12, COL_CARD);
  lcd.fillRoundRect(SCR_W - 40, trackY + 2, 12, 4, 2, TFT_WHITE);
  lcd.fillRoundRect(SCR_W - 35, trackY - 2, 3, 12, 2, TFT_WHITE);

  lcd.fillRoundRect(trackX, trackY, trackW, trackH, 4, COL_CARD);
  int fill = (volumeLevel * trackW) / 21;
  if (fill > 0) lcd.fillRoundRect(trackX, trackY, fill, trackH, 4, COL_ACCENT);
  int thumbX = trackX + fill;
  if (thumbX < trackX + 6) thumbX = trackX + 6;
  if (thumbX > trackX + trackW - 6) thumbX = trackX + trackW - 6;
  lcd.fillCircle(thumbX, trackY + 4, 6, TFT_WHITE);
}

static void drawFeatureBar() {
  lcd.fillRect(12, 138, SCR_W - 24, 28, COL_BG);
  lcd.fillRoundRect(12, 140, 92, 24, 8, COL_CARD);
  lcd.fillRoundRect(114, 140, 92, 24, 8, COL_CARD);
  lcd.fillRoundRect(216, 140, 92, 24, 8, COL_CARD);

  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("Wi-Fi", 58, 152);
  lcd.drawString("Stations", 160, 152);

  char sleepBtn[12];
  if (sleepMinutes <= 0) snprintf(sleepBtn, sizeof(sleepBtn), "Sleep");
  else snprintf(sleepBtn, sizeof(sleepBtn), "%dm", sleepMinutes);
  lcd.drawString(sleepBtn, 262, 152);
}

static void drawTransport() {
  lcd.fillRect(12, 170, SCR_W - 24, 64, COL_BG);

  lcd.fillTriangle(70 - 14, 200, 70 - 2, 190, 70 - 2, 210, COL_ACCENT);
  lcd.fillTriangle(70 - 4, 200, 70 + 8, 190, 70 + 8, 210, COL_ACCENT);

  lcd.fillCircle(160, 200, 22, COL_PLAY);
  if (isPlaying) {
    lcd.fillRect(151, 190, 6, 20, TFT_WHITE);
    lcd.fillRect(163, 190, 6, 20, TFT_WHITE);
  } else {
    lcd.fillTriangle(154, 188, 154, 212, 174, 200, TFT_WHITE);
  }

  lcd.fillTriangle(250 + 14, 200, 250 + 2, 190, 250 + 2, 210, COL_ACCENT);
  lcd.fillTriangle(250 + 4, 200, 250 - 8, 190, 250 - 8, 210, COL_ACCENT);
  dockDirty = false;
}

static void drawUi() {
  if (stationDirty) {
    lcd.fillScreen(COL_BG);
    headerDirty = true;
    dockDirty = true;
    drawStationCard();
  }
  if (headerDirty) drawHeader();
  if (statusDirty) drawStatus();
  if (dockDirty) {
    drawVolume();
    drawFeatureBar();
    drawTransport();
  }
}

static void initTouchController() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  pinMode(TOUCH_INT, INPUT);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
}

static bool readTouchScreen(int32_t &sx, int32_t &sy) {
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

static bool waitForTouch(int32_t &sx, int32_t &sy) {
  static bool down = false;
  while (true) {
    bool touching = readTouchScreen(sx, sy);
    if (touching && !down) {
      down = true;
      bumpActivity();
      delay(40);
      return true;
    }
    if (!touching) down = false;
    delay(12);
  }
}

static void connectCurrentStation();
static void skipToNextStation();
static void skipToPrevStation();
static bool runWifiSetupUI();
static void runStationListUI();
static void cycleSleepTimer();

static void stationConnectTask(void* param) {
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

      if (gen != connectGen || idx != currentStationIdx) {
        streamConnectMs = millis();
        setStatus("Connecting...");
        continue;
      }
      break;
    }
  }
}

static void initAudioHardware() {
  pinMode(I2S_AMP_EN, OUTPUT);
  digitalWrite(I2S_AMP_EN, LOW);
  delay(30);
  if (!initES8311Codec()) {
    setStatus("Codec error");
    return;
  }
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volumeLevel);
  uint8_t reg = (volumeLevel == 0) ? 0 : (uint8_t)((volumeLevel * 255) / 21);
  es8311Write(0x32, reg);
  audio.setConnectionTimeout(2500, 3000);
  audioReady = true;

  if (!audioMutex) audioMutex = xSemaphoreCreateMutex();
  if (!stationTaskHandle) {
    xTaskCreatePinnedToCore(stationConnectTask, "station", 8192, nullptr, 1, &stationTaskHandle, 0);
  }
}

static void connectCurrentStation() {
  if (!audioReady || totalStations <= 0) return;
  if (currentStationIdx < 0) currentStationIdx = 0;
  if (currentStationIdx >= totalStations) currentStationIdx = 0;
  saveStation();
  userPaused = false;
  isPlaying = true;
  streamPlaying = false;
  streamTitle[0] = '\0';
  streamConnectMs = millis();
  setStatus("Connecting...");
  stationDirty = true;
  dockDirty = true;
  connectGen++;
  if (stationTaskHandle) xTaskNotifyGive(stationTaskHandle);
}

static void skipToNextStation() {
  if (totalStations <= 0) return;
  currentStationIdx = (currentStationIdx + 1) % totalStations;
  connectCurrentStation();
}

static void skipToPrevStation() {
  if (totalStations <= 0) return;
  currentStationIdx = (currentStationIdx - 1 + totalStations) % totalStations;
  connectCurrentStation();
}

static void cycleSleepTimer() {
  static const int options[] = {0, 15, 30, 60};
  int next = 0;
  for (int i = 0; i < 4; i++) {
    if (options[i] == sleepMinutes) {
      next = options[(i + 1) % 4];
      break;
    }
  }
  sleepMinutes = next;
  if (sleepMinutes == 0) {
    sleepUntilMs = 0;
    setStatus("Sleep off");
  } else {
    sleepUntilMs = millis() + (unsigned long)sleepMinutes * 60000UL;
    char msg[24];
    snprintf(msg, sizeof(msg), "Sleep %dm", sleepMinutes);
    setStatus(msg);
  }
  headerDirty = true;
  dockDirty = true;
}

static bool connectToWifi(const char* ssid, const char* password, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(80);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    return true;
  }
  return false;
}

static bool hasInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin("http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();
  return code == 204 || code == 200 || code == 301 || code == 302;
}

static void scanWifiNetworks() {
  drawMessage("Scanning Wi-Fi", "Please wait");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(100);
  int n = WiFi.scanNetworks(false, false);
  scannedCount = 0;
  wifiListOffset = 0;
  for (int i = 0; i < n && scannedCount < MAX_WIFI_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool exists = false;
    for (int j = 0; j < scannedCount; j++) {
      if (strcmp(scannedSsids[j], ssid.c_str()) == 0) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      strncpy(scannedSsids[scannedCount], ssid.c_str(), 32);
      scannedSsids[scannedCount][32] = '\0';
      scannedCount++;
    }
  }
  WiFi.scanDelete();
}

static void drawWifiScanScreen() {
  lcd.fillScreen(COL_BG);
  lcd.fillRoundRect(8, 6, SCR_W - 16, 28, 8, COL_CARD);
  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(COL_ACCENT);
  lcd.drawString("Wi-Fi Setup — tap a network", 18, 20);

  if (scannedCount == 0) {
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(COL_MUTED);
    lcd.drawString("No networks found", SCR_W / 2, 100);
  } else {
    int visible = min(5, scannedCount - wifiListOffset);
    for (int i = 0; i < visible; i++) {
      int idx = wifiListOffset + i;
      int y = 42 + i * 28;
      lcd.fillRoundRect(8, y, SCR_W - 16, 26, 6, COL_CARD);
      lcd.setTextDatum(textdatum_t::middle_left);
      lcd.setTextColor(TFT_WHITE);
      lcd.drawString(scannedSsids[idx], 18, y + 13);
    }
  }

  lcd.fillRoundRect(8, 190, 70, 40, 8, COL_ACCENT);
  lcd.fillRoundRect(86, 190, 70, 40, 8, COL_CARD);
  lcd.fillRoundRect(164, 190, 70, 40, 8, COL_CARD);
  lcd.fillRoundRect(242, 190, 70, 40, 8, COL_DANGER);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("SCAN", 43, 210);
  lcd.drawString("UP", 121, 210);
  lcd.drawString("DOWN", 199, 210);
  lcd.drawString("SKIP", 277, 210);
}

static bool promptWifiPassword(const char* ssid, char* outPassword, size_t outLen) {
  static const char* rows[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm"
  };
  const int rowCount = 4;
  char password[64] = "";
  bool shifted = false;

  auto redraw = [&]() {
    lcd.fillScreen(COL_BG);
    lcd.fillRoundRect(8, 4, SCR_W - 16, 28, 8, COL_CARD);
    lcd.setFont(&fonts::DejaVu12);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setTextColor(COL_ACCENT);
    char title[40];
    snprintf(title, sizeof(title), "Password: %.22s", ssid);
    lcd.drawString(title, 16, 18);

    lcd.fillRoundRect(8, 38, SCR_W - 16, 26, 6, COL_CARD);
    lcd.setTextColor(TFT_WHITE);
    if (password[0]) {
      char shown[40];
      size_t len = strlen(password);
      if (len > 28) snprintf(shown, sizeof(shown), "%s", password + (len - 28));
      else snprintf(shown, sizeof(shown), "%s", password);
      lcd.drawString(shown, 16, 51);
    } else {
      lcd.setTextColor(COL_MUTED);
      lcd.drawString("(empty / open network)", 16, 51);
    }

    for (int r = 0; r < rowCount; r++) {
      const char* keys = rows[r];
      int keyCount = (int)strlen(keys);
      int keyW = 28;
      int startX = (SCR_W - keyCount * keyW) / 2;
      int y = 72 + r * 28;
      for (int c = 0; c < keyCount; c++) {
        char ch = keys[c];
        if (shifted && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        int x = startX + c * keyW;
        lcd.fillRoundRect(x, y, keyW - 2, 24, 4, COL_CARD);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.setTextColor(TFT_WHITE);
        char label[2] = { ch, 0 };
        lcd.drawString(label, x + (keyW - 2) / 2, y + 12);
      }
    }

    lcd.fillRoundRect(8, 192, 56, 36, 8, COL_WARN);
    lcd.fillRoundRect(70, 192, 56, 36, 8, COL_DANGER);
    lcd.fillRoundRect(132, 192, 100, 36, 8, COL_CARD);
    lcd.fillRoundRect(238, 192, 74, 36, 8, COL_OK);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(TFT_WHITE);
    lcd.drawString(shifted ? "ABC" : "abc", 36, 210);
    lcd.drawString("DEL", 98, 210);
    lcd.drawString("SPACE", 182, 210);
    lcd.drawString("OK", 275, 210);
  };

  redraw();
  while (true) {
    int32_t x, y;
    if (!waitForTouch(x, y)) continue;

    if (y >= 192) {
      if (x < 64) {
        shifted = !shifted;
        redraw();
      } else if (x < 126) {
        size_t len = strlen(password);
        if (len > 0) {
          password[len - 1] = '\0';
          redraw();
        }
      } else if (x < 232) {
        size_t len = strlen(password);
        if (len < sizeof(password) - 1) {
          password[len] = ' ';
          password[len + 1] = '\0';
          redraw();
        }
      } else {
        strncpy(outPassword, password, outLen - 1);
        outPassword[outLen - 1] = '\0';
        return true;
      }
      continue;
    }

    if (y >= 72 && y < 184) {
      int r = (y - 72) / 28;
      if (r >= 0 && r < rowCount) {
        const char* keys = rows[r];
        int keyCount = (int)strlen(keys);
        int keyW = 28;
        int startX = (SCR_W - keyCount * keyW) / 2;
        int c = (x - startX) / keyW;
        size_t len = strlen(password);
        if (c >= 0 && c < keyCount && len < sizeof(password) - 1) {
          char ch = keys[c];
          if (shifted && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
          password[len] = ch;
          password[len + 1] = '\0';
          redraw();
        }
      }
    }
  }
}

static bool runWifiSetupUI() {
  if (audioReady && audioLock(portMAX_DELAY)) {
    audio.stopSong();
    audioUnlock();
  }
  streamPlaying = false;
  scanWifiNetworks();
  drawWifiScanScreen();

  while (true) {
    int32_t x, y;
    if (!waitForTouch(x, y)) continue;

    if (y >= 190) {
      if (x < 78) {
        scanWifiNetworks();
        drawWifiScanScreen();
      } else if (x < 156) {
        if (wifiListOffset > 0) {
          wifiListOffset--;
          drawWifiScanScreen();
        }
      } else if (x < 234) {
        if (wifiListOffset + 5 < scannedCount) {
          wifiListOffset++;
          drawWifiScanScreen();
        }
      } else {
        return WiFi.status() == WL_CONNECTED;
      }
      continue;
    }

    if (y >= 42 && y < 182 && scannedCount > 0) {
      int row = (y - 42) / 28;
      int idx = wifiListOffset + row;
      if (row >= 0 && row < 5 && idx >= 0 && idx < scannedCount) {
        char password[64];
        if (!promptWifiPassword(scannedSsids[idx], password, sizeof(password))) {
          drawWifiScanScreen();
          continue;
        }
        drawMessage("Connecting...", scannedSsids[idx]);
        if (connectToWifi(scannedSsids[idx], password, WIFI_TIMEOUT_MS)) {
          saveWifiCredentials(scannedSsids[idx], password);
          drawMessage("Connected", scannedSsids[idx]);
          delay(700);
          return true;
        }
        drawMessage("Connect failed", "Check password");
        delay(1000);
        drawWifiScanScreen();
      }
    }
  }
}

static void drawStationListScreen() {
  lcd.fillScreen(COL_BG);
  lcd.fillRoundRect(8, 6, SCR_W - 16, 28, 8, COL_CARD);
  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(COL_ACCENT);
  lcd.drawString("Stations — tap to play", 18, 20);

  if (totalStations <= 0) {
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(COL_MUTED);
    lcd.drawString("No stations loaded", SCR_W / 2, 100);
  } else {
    int visible = min(5, totalStations - listOffset);
    for (int i = 0; i < visible; i++) {
      int idx = listOffset + i;
      int y = 42 + i * 28;
      uint16_t bg = (idx == currentStationIdx) ? C(40, 70, 110) : COL_CARD;
      lcd.fillRoundRect(8, y, SCR_W - 16, 26, 6, bg);
      lcd.setTextDatum(textdatum_t::middle_left);
      lcd.setTextColor(TFT_WHITE);
      char line[40];
      snprintf(line, sizeof(line), "%d. %s", idx + 1, playlist[idx].name);
      lcd.drawString(line, 18, y + 13);
    }
  }

  lcd.fillRoundRect(8, 190, 96, 40, 8, COL_CARD);
  lcd.fillRoundRect(112, 190, 96, 40, 8, COL_CARD);
  lcd.fillRoundRect(216, 190, 96, 40, 8, COL_DANGER);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("UP", 56, 210);
  lcd.drawString("DOWN", 160, 210);
  lcd.drawString("BACK", 264, 210);
}

static void runStationListUI() {
  if (currentStationIdx >= 5) listOffset = currentStationIdx - 2;
  if (listOffset < 0) listOffset = 0;
  if (listOffset > max(0, totalStations - 5)) listOffset = max(0, totalStations - 5);
  drawStationListScreen();

  while (true) {
    int32_t x, y;
    if (!waitForTouch(x, y)) continue;

    if (y >= 190) {
      if (x < 104) {
        if (listOffset > 0) {
          listOffset--;
          drawStationListScreen();
        }
      } else if (x < 208) {
        if (listOffset + 5 < totalStations) {
          listOffset++;
          drawStationListScreen();
        }
      } else {
        stationDirty = true;
        dockDirty = true;
        headerDirty = true;
        return;
      }
      continue;
    }

    if (y >= 42 && y < 182 && totalStations > 0) {
      int row = (y - 42) / 28;
      int idx = listOffset + row;
      if (row >= 0 && row < 5 && idx >= 0 && idx < totalStations) {
        currentStationIdx = idx;
        connectCurrentStation();
        stationDirty = true;
        dockDirty = true;
        headerDirty = true;
        return;
      }
    }
  }
}

static void handleTouch(int32_t tx, int32_t ty) {
  bumpActivity();

  if (ty >= 108 && ty <= 134) {
    if (tx <= 50) {
      setVolumeLevel(volumeLevel - 1);
      return;
    }
    if (tx >= SCR_W - 50) {
      setVolumeLevel(volumeLevel + 1);
      return;
    }
    if (tx >= 56 && tx <= 224) {
      setVolumeLevel(((tx - 56) * 21) / 168);
    }
    return;
  }

  if (ty >= 138 && ty <= 166) {
    if (tx < 110) {
      if (runWifiSetupUI()) {
        syncNtpClock();
        if (!loadPlaylistFromGitHub() && totalStations <= 0) {
          if (!loadPlaylistFromCache()) loadBuiltinStations();
        }
        if (audioReady && totalStations > 0) connectCurrentStation();
      }
      stationDirty = true;
      dockDirty = true;
      headerDirty = true;
      return;
    }
    if (tx < 214) {
      runStationListUI();
      return;
    }
    cycleSleepTimer();
    return;
  }

  if (ty < 174) {
    if (ty >= 26 && ty <= 104) {
      runStationListUI();
    }
    return;
  }

  if (tx >= 34 && tx <= 106) {
    skipToPrevStation();
  } else if (tx >= 124 && tx <= 196) {
    if (audioReady && !stationConnecting && audioLock(0)) {
      isPlaying = !isPlaying;
      userPaused = !isPlaying;
      audio.pauseResume();
      audioUnlock();
      dockDirty = true;
      statusDirty = true;
    }
  } else if (tx >= 214 && tx <= 286) {
    skipToNextStation();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(I2S_AMP_EN, OUTPUT);
  digitalWrite(I2S_AMP_EN, HIGH);
  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);
  pinMode(BAT_ADC_PIN, INPUT);

  lcd.init();
  lcd.setBrightness(BRIGHT_FULL);
  lcd.setRotation(1);
  initTouchController();
  updateBatteryReading();
  drawMessage("Internet Radio", "Booting...");

  if (!psramFound()) {
    drawMessage("PSRAM not found", "Enable OPI PSRAM");
    return;
  }

  fsReady = LittleFS.begin(true);
  loadSettings();
  lastTouchMs = millis();

  drawMessage("Internet Radio", "Connecting Wi-Fi...");
  bool wifiOk = connectToWifi(wifiSsid, wifiPass, WIFI_TIMEOUT_MS);
  if (!wifiOk || !hasInternet()) {
    drawMessage("Wi-Fi needed", "Opening setup...");
    delay(600);
    wifiOk = runWifiSetupUI();
  }

  if (wifiOk) {
    drawMessage("Internet Radio", "Syncing clock...");
    syncNtpClock();
    drawMessage("Internet Radio", "Loading stations...");
    if (!loadPlaylistFromGitHub()) {
      if (!loadPlaylistFromCache()) {
        loadBuiltinStations();
        Serial.println("Using built-in stations");
      } else {
        Serial.println("Using cached stations");
      }
    }
  } else if (!loadPlaylistFromCache()) {
    loadBuiltinStations();
    drawMessage("Offline mode", "Built-in stations");
    delay(800);
  }

  Serial.printf("Stations: %d\n", totalStations);
  if (currentStationIdx < 0 || currentStationIdx >= totalStations) currentStationIdx = 0;

  initAudioHardware();
  drawUi();
  if (totalStations > 0) connectCurrentStation();
}

void loop() {
  if (!audioReady && WiFi.status() != WL_CONNECTED && totalStations <= 0) return;

  static bool touchDown = false;
  int32_t touchX, touchY;
  bool touching = readTouchScreen(touchX, touchY);
  if (touching && !touchDown && (millis() - lastDebounceTime) > 200) {
    handleTouch(touchX, touchY);
    lastDebounceTime = millis();
  }
  touchDown = touching;

  if (millis() - lastBatteryMs > 5000) {
    lastBatteryMs = millis();
    updateBatteryReading();
  }

  if (millis() - lastClockMs > 15000) {
    lastClockMs = millis();
    updateClockText();
  }

  if (!screenDimmed && (millis() - lastTouchMs > IDLE_DIM_MS)) {
    lcd.setBrightness(BRIGHT_DIM);
    screenDimmed = true;
  }

  if (sleepMinutes > 0 && sleepUntilMs > 0 && (long)(millis() - sleepUntilMs) >= 0) {
    sleepMinutes = 0;
    sleepUntilMs = 0;
    if (audioReady && isPlaying && audioLock(0)) {
      isPlaying = false;
      userPaused = true;
      audio.pauseResume();
      audioUnlock();
    }
    setStatus("Sleep ended");
    headerDirty = true;
    dockDirty = true;
  }

  if (audioReady && !userPaused && !stationConnecting && !streamPlaying &&
      streamConnectMs > 0 && (millis() - streamConnectMs > STREAM_SKIP_MS)) {
    setStatus("Skipping...");
    skipToNextStation();
  }

  drawUi();

  if (audioReady && audioLock(0)) {
    audio.loop();
    if (audio.isRunning() && !streamPlaying) {
      streamPlaying = true;
      userPaused = false;
      if (!streamTitle[0]) setStatus("Now playing");
      statusDirty = true;
    }
    audioUnlock();
  }
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
    setStatus("Now playing");
  } else if (msg.indexOf("failed") >= 0 || msg.indexOf("404") >= 0 || msg.indexOf("timeout") >= 0) {
    setStatus("Stream error");
    if (millis() - lastAutoSkipMs > 3000) {
      lastAutoSkipMs = millis();
      skipToNextStation();
    }
  }
}
