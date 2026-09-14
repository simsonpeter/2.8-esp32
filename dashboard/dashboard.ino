/*
 * Simple internet radio — ES3C28P 2.8" ESP32-S3 (320x240)
 * Wiki: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
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

#define SCR_W         320
#define SCR_H         240
#define MAX_STATIONS  40
#define NAME_LEN      28
#define URL_LEN       512
#define WIFI_TIMEOUT_MS 20000
#define STREAM_SKIP_MS  12000

#define C(r, g, b) lgfx::color565(r, g, b)
#define COL_BG     C(16, 20, 32)
#define COL_CARD   C(28, 34, 52)
#define COL_ACCENT C(88, 166, 255)
#define COL_PLAY   C(255, 107, 107)
#define COL_MUTED  C(140, 148, 168)
#define COL_OK     C(46, 204, 113)

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

static char wifiSsid[33] = "simson";
static char wifiPass[64] = "jayatha10";
static const char* jsonUrl = "https://raw.githubusercontent.com/simsonpeter/Tcradios/refs/heads/main/stations.json";

static char statusLine[40] = "Starting...";
static char streamTitle[40] = "";
static bool isPlaying = true;
static bool audioReady = false;
static bool streamPlaying = false;
static bool userPaused = false;
static volatile bool stationConnecting = false;
static volatile int connectGen = 0;
static unsigned long streamConnectMs = 0;
static unsigned long lastAutoSkipMs = 0;
static unsigned long lastDebounceTime = 0;

static SemaphoreHandle_t audioMutex = nullptr;
static TaskHandle_t stationTaskHandle = nullptr;

static bool stationDirty = true;
static bool statusDirty = true;
static bool dockDirty = true;

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

static bool extractJsonField(const char* json, const char* field, char* dest, size_t destLen) {
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

static bool parsePlaylistJson(const char* json) {
  if (!json || !json[0]) return false;
  totalStations = 0;
  const char* cursor = json;
  while (totalStations < MAX_STATIONS) {
    const char* obj = strstr(cursor, "\"name\"");
    if (!obj) break;
    char name[NAME_LEN], url[URL_LEN];
    if (!extractJsonField(obj, "name", name, sizeof(name))) break;
    if (!extractJsonField(obj, "url", url, sizeof(url)) || !url[0]) {
      cursor = obj + 6;
      continue;
    }
    addStation(name, url);
    const char* nextUrl = strstr(obj, "\"url\"");
    cursor = nextUrl ? nextUrl + 5 : obj + 6;
  }
  return totalStations > 0;
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
  return parsePlaylistJson(payload.c_str());
}

static void loadSettings() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", wifiSsid);
  String p = prefs.getString("pass", wifiPass);
  prefs.end();
  strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
  strncpy(wifiPass, p.c_str(), sizeof(wifiPass) - 1);

  prefs.begin("audio", true);
  volumeLevel = prefs.getInt("volume", 12);
  currentStationIdx = prefs.getInt("station", 0);
  prefs.end();
  if (volumeLevel < 0) volumeLevel = 0;
  if (volumeLevel > 21) volumeLevel = 21;
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

static void drawStationCard() {
  lcd.fillRoundRect(12, 12, SCR_W - 24, 96, 14, COL_CARD);

  lcd.setFont(&fonts::DejaVu12);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(COL_ACCENT);
  lcd.drawString("Internet Radio", 24, 22);

  char idx[12];
  snprintf(idx, sizeof(idx), "%d / %d", totalStations ? currentStationIdx + 1 : 0, totalStations);
  lcd.setTextDatum(textdatum_t::top_right);
  lcd.setTextColor(COL_MUTED);
  lcd.drawString(idx, SCR_W - 24, 22);

  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(TFT_WHITE);
  const char* name = totalStations > 0 ? playlist[currentStationIdx].name : "No stations";
  lcd.drawString(name, 24, 48);

  stationDirty = false;
  statusDirty = true;
}

static void drawStatus() {
  lcd.fillRect(24, 78, SCR_W - 48, 22, COL_CARD);
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
    badgeCol = C(255, 177, 66);
  }

  lcd.setTextColor(badgeCol);
  lcd.drawString(badge, 24, 80);
  lcd.setTextColor(COL_MUTED);
  lcd.drawString(streamTitle[0] ? streamTitle : statusLine, 92, 80);
  statusDirty = false;
}

static void drawVolume() {
  const int trackX = 56;
  const int trackY = 132;
  const int trackW = 168;
  const int trackH = 8;

  lcd.fillRect(12, 118, SCR_W - 24, 36, COL_BG);
  lcd.fillCircle(34, trackY + 4, 14, COL_CARD);
  lcd.fillRoundRect(27, trackY + 2, 14, 4, 2, TFT_WHITE);
  lcd.fillCircle(SCR_W - 34, trackY + 4, 14, COL_CARD);
  lcd.fillRoundRect(SCR_W - 41, trackY + 2, 14, 4, 2, TFT_WHITE);
  lcd.fillRoundRect(SCR_W - 36, trackY - 3, 4, 14, 2, TFT_WHITE);

  lcd.fillRoundRect(trackX, trackY, trackW, trackH, 4, COL_CARD);
  int fill = (volumeLevel * trackW) / 21;
  if (fill > 0) lcd.fillRoundRect(trackX, trackY, fill, trackH, 4, COL_ACCENT);
  int thumbX = trackX + fill;
  if (thumbX < trackX + 6) thumbX = trackX + 6;
  if (thumbX > trackX + trackW - 6) thumbX = trackX + trackW - 6;
  lcd.fillCircle(thumbX, trackY + 4, 6, TFT_WHITE);
}

static void drawTransport() {
  lcd.fillRect(12, 160, SCR_W - 24, 72, COL_BG);

  lcd.fillTriangle(70 - 14, 196, 70 - 2, 186, 70 - 2, 206, COL_ACCENT);
  lcd.fillTriangle(70 - 4, 196, 70 + 8, 186, 70 + 8, 206, COL_ACCENT);

  lcd.fillCircle(160, 196, 24, COL_PLAY);
  if (isPlaying) {
    lcd.fillRect(151, 186, 6, 20, TFT_WHITE);
    lcd.fillRect(163, 186, 6, 20, TFT_WHITE);
  } else {
    lcd.fillTriangle(154, 184, 154, 208, 174, 196, TFT_WHITE);
  }

  lcd.fillTriangle(250 + 14, 196, 250 + 2, 186, 250 + 2, 206, COL_ACCENT);
  lcd.fillTriangle(250 + 4, 196, 250 - 8, 186, 250 - 8, 206, COL_ACCENT);
  dockDirty = false;
}

static void drawUi() {
  if (stationDirty) {
    lcd.fillScreen(COL_BG);
    drawStationCard();
    dockDirty = true;
  }
  if (statusDirty) drawStatus();
  if (dockDirty) {
    drawVolume();
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

static void connectCurrentStation();
static void skipToNextStation();
static void skipToPrevStation();

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

static void handleTouch(int32_t tx, int32_t ty) {
  if (ty >= 118 && ty <= 154) {
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

  if (ty < 164) return;

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

  lcd.init();
  lcd.setBrightness(255);
  lcd.setRotation(1);
  initTouchController();
  drawMessage("Internet Radio", "Connecting Wi-Fi...");

  if (!psramFound()) {
    drawMessage("PSRAM not found", "Enable OPI PSRAM");
    return;
  }

  loadSettings();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawMessage("Wi-Fi failed", wifiSsid);
    return;
  }
  WiFi.setSleep(false);

  drawMessage("Internet Radio", "Loading stations...");
  if (!loadPlaylistFromGitHub()) {
    loadBuiltinStations();
    Serial.println("Using built-in stations");
  }
  Serial.printf("Stations: %d\n", totalStations);
  if (currentStationIdx < 0 || currentStationIdx >= totalStations) currentStationIdx = 0;

  initAudioHardware();
  drawUi();
  if (totalStations > 0) connectCurrentStation();
}

void loop() {
  if (!audioReady && WiFi.status() != WL_CONNECTED) return;

  static bool touchDown = false;
  int32_t touchX, touchY;
  bool touching = readTouchScreen(touchX, touchY);
  if (touching && !touchDown && (millis() - lastDebounceTime) > 200) {
    handleTouch(touchX, touchY);
    lastDebounceTime = millis();
  }
  touchDown = touching;

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
