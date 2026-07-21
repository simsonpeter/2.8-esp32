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
int volumeLevel = 18;
unsigned long streamConnectMs = 0;
unsigned long lastAutoSkipMs = 0;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

const char* ssid = "simson";
const char* password = "jayatha10";
const char* jsonUrl = "https://raw.githubusercontent.com/simsonpeter/Tcradios/refs/heads/main/stations.json";

enum BootState { BOOT_SPLASH, BOOT_WIFI, BOOT_PLAYLIST, BOOT_AUDIO, BOOT_DONE };
BootState bootState = BOOT_SPLASH;
unsigned long wifiStartMs = 0;
unsigned long splashStartMs = 0;
bool playlistLoadStarted = false;
bool playlistRefreshPending = false;
bool fsReady = false;

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
  volumeLevel = v;
  if (audioReady) audio.setVolume(volumeLevel);
  uint8_t reg = (v == 0) ? 0 : (uint8_t)((v * 255) / 21);
  es8311Write(0x32, reg);
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

  lcd.fillRect(200, 10, 50, 22, C(255, 255, 255));
  if (streamPlaying && isPlaying) {
    lcd.fillRoundRect(202, 12, 44, 18, 8, C(0, 184, 148));
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(1);
    lcd.drawString("LIVE", 210, 17);
  }

  if (totalStations > 0) {
    char idx[12];
    snprintf(idx, sizeof(idx), "%d/%d", currentStationIdx + 1, totalStations);
    lcd.fillRect(SCR_W - 60, 10, 52, 18, C(255, 255, 255));
    lcd.setTextColor(C(120, 120, 140));
    lcd.setTextSize(1);
    int tw = lcd.textWidth(idx);
    lcd.drawString(idx, SCR_W - tw - 18, 16);
  }

  if (totalStations > 0) {
    lcd.fillRect(98, 32, 210, 52, C(255, 255, 255));
    lcd.setTextColor(C(40, 40, 60));
    lcd.setTextSize(2);
    lcd.drawString(playlist[currentStationIdx].name, 100, 36);
    lcd.setTextColor(C(253, 121, 168));
    lcd.setTextSize(1);
    lcd.drawString(playlist[currentStationIdx].genre, 100, 64);
  }

  stationDirty = false;
}

void drawLiveBadgeOnly() {
  lcd.fillRect(200, 10, 50, 22, C(255, 255, 255));
  if (streamPlaying && isPlaying) {
    lcd.fillRoundRect(202, 12, 44, 18, 8, C(0, 184, 148));
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(1);
    lcd.drawString("LIVE", 210, 17);
  }
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

void handleTouch(int32_t tx, int32_t ty) {
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
      if (audioReady) {
        isPlaying = !isPlaying;
        userPaused = !isPlaying;
        audio.pauseResume();
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

  fsReady = LittleFS.begin(true);
  if (fsReady) loadPlaylistFromCache();

  splashStartMs = millis();
  wifiStartMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

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
        bootState = (totalStations > 0) ? BOOT_AUDIO : BOOT_DONE;
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

  if (audioReady) { audio.loop(); refreshPlaybackStatus(); }

  if (bootState == BOOT_DONE && playlistRefreshPending && WiFi.status() == WL_CONNECTED) {
    playlistRefreshPending = false;
    int prevIdx = currentStationIdx;
    if (loadPlaylistFromGitHub()) {
      if (currentStationIdx >= totalStations) currentStationIdx = 0;
      stationDirty = true;
      if (prevIdx != currentStationIdx && audioReady) connectCurrentStation();
    }
  }

  static bool touchDown = false;
  int32_t touchX, touchY;
  bool touching = (bootState == BOOT_DONE) && readTouchScreen(touchX, touchY);
  if (touching && !touchDown && (millis() - lastDebounceTime) > debounceDelay) {
    handleTouch(touchX, touchY);
    lastDebounceTime = millis();
  }
  touchDown = touching;

  if (bootState == BOOT_DONE && audioReady && !userPaused &&
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
  audio.setConnectionTimeout(4000, 5000);
  audioReady = true;
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
  audio.stopSong();
  userPaused = false;
  isPlaying = true;
  streamPlaying = false;
  streamTitle[0] = '\0';
  streamConnectMs = millis();
  setStatus("Connecting...");
  stationDirty = true;
  statusDirty = true;
  audio.connecttohost(playlist[currentStationIdx].url);
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
