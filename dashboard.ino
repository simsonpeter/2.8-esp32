#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include "es8311.h"

// --- HARDWARE CONFIGURATION (Freenove FNK0104AB-compatible 2.8" ILI9341) ---
#define TFT_MOSI      11
#define TFT_MISO      13
#define TFT_SCLK      12
#define TFT_DC        46
#define TFT_CS        10
#define TFT_BL        45

#define TOUCH_SDA     16
#define TOUCH_SCL     15
#define TOUCH_INT     17
#define TOUCH_RST     18

#define I2S_MCLK      GPIO_NUM_4
#define I2S_BCLK      GPIO_NUM_5
#define I2S_DIN       GPIO_NUM_6
#define I2S_LRCK      GPIO_NUM_7
#define I2S_DOUT      GPIO_NUM_8
#define AUDIO_AMP_EN  GPIO_NUM_1
#define AUDIO_I2C_HZ  400000

#define MAX_STATIONS 20

struct RadioStation {
  String name;
  String url;
};

RadioStation playlist[MAX_STATIONS];
int totalStations = 0;
int currentStationIdx = 0;
Preferences preferences;

// --- DISPLAY SETUP ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341  _panel_instance;
  lgfx::Bus_SPI        _bus_instance;
  lgfx::Touch_FT5x06   _touch_instance; 
  lgfx::Light_PWM      _light_instance; 
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.pin_mosi = TFT_MOSI; cfg.pin_miso = TFT_MISO; cfg.pin_sclk = TFT_SCLK; cfg.pin_dc = TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = TFT_CS; cfg.pin_rst = -1;
      cfg.panel_width = 240; cfg.panel_height = 320;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = TFT_BL;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0; cfg.x_max = 239; cfg.y_min = 0; cfg.y_max = 319;
      cfg.pin_sda = TOUCH_SDA; cfg.pin_scl = TOUCH_SCL; cfg.pin_int = TOUCH_INT; cfg.pin_rst = TOUCH_RST;
      cfg.i2c_port = 0; 
      cfg.freq = 400000; 
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

static LGFX lcd;
Audio audio;
I2SClass es8311I2S;

String currentTrack = "Connecting...";
bool isPlaying = true;
bool audioHardwareReady = false;

// Network Profiles
const char* ssid = "simson";
const char* password = "jayatha10";
const char* jsonUrl = "https://raw.githubusercontent.com/simsonpeter/Tcradios/refs/heads/main/stations.json";

// Non-blocking input management
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 400; 

// Forward Declarations
void loadPlaylistFromGitHub();
void drawBaseUI();
void updateDisplayStrings();
bool initAudioHardware();
void saveLastPlayedStation();
void loadLastPlayedStation();

void setup() {
  Serial.begin(115200);
  
  lcd.init();
  lcd.setRotation(1); 
  
  uint16_t calData[8] = { 240, 0, 0, 0, 240, 320, 0, 320 };
  lcd.setTouchCalibrate(calData);
  
  drawBaseUI();
  
  lcd.fillRect(10, 60, 300, 30, TFT_BLACK);
  lcd.setTextColor(TFT_CYAN);
  lcd.setTextSize(2);
  lcd.drawString("Loading Playlist...", 15, 60);

  lcd.fillRect(10, 100, 300, 50, TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1.5);
  lcd.drawString("Connecting to Wi-Fi...", 15, 105);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(250); 
    Serial.print(".");
  }
  
  loadPlaylistFromGitHub();
  loadLastPlayedStation();

  if (totalStations > 0) {
    currentTrack = "Connecting to audio stream...";
  } else {
    currentTrack = "Error: Playlist empty";
  }
  updateDisplayStrings();

  audioHardwareReady = initAudioHardware();
  if (!audioHardwareReady) {
    currentTrack = "Audio codec init failed";
    updateDisplayStrings();
  }

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
  audio.setVolume(12); 
  
  // Always resume the last played radio station on start
  if (audioHardwareReady && totalStations > 0) {
    audio.connecttohost(playlist[currentStationIdx].url.c_str());
  }
}

void loop() {
  audio.loop();

  int32_t touchX, touchY;
  if (lcd.getTouch(&touchX, &touchY)) {
    if ((millis() - lastDebounceTime) > debounceDelay) {
      if (touchY > 160 && touchY < 220 && touchX > 20 && touchX < 90) {
        if (audioHardwareReady && totalStations > 0) {
          currentStationIdx = (currentStationIdx - 1 + totalStations) % totalStations;
          saveLastPlayedStation();
          audio.connecttohost(playlist[currentStationIdx].url.c_str());
          currentTrack = "Loading stream...";
          updateDisplayStrings();
          lastDebounceTime = millis();
        }
      }
      else if (touchY > 160 && touchY < 220 && touchX > 120 && touchX < 190 && audioHardwareReady) {
        isPlaying = !isPlaying;
        audio.pauseResume(); 
        lastDebounceTime = millis();
      }
      else if (touchY > 160 && touchY < 220 && touchX > 220 && touchX < 290) {
        if (audioHardwareReady && totalStations > 0) {
          currentStationIdx = (currentStationIdx + 1) % totalStations;
          saveLastPlayedStation();
          audio.connecttohost(playlist[currentStationIdx].url.c_str());
          currentTrack = "Loading stream...";
          updateDisplayStrings();
          lastDebounceTime = millis();
        }
      }
    }
  }

  static unsigned long lastUIUpdate = 0;
  if (millis() - lastUIUpdate > 1000) {
    lastUIUpdate = millis();
    updateDisplayStrings();
  }
}

void loadPlaylistFromGitHub() {
  HTTPClient http;
  http.begin(jsonUrl);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    DynamicJsonDocument doc(4096); 
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject obj : arr) {
        if (totalStations >= MAX_STATIONS) break;
        
        playlist[totalStations].name = obj["name"].as<String>();
        playlist[totalStations].url = obj["url"].as<String>();
        totalStations++;
      }
      Serial.printf("Successfully parsed %d stations.\n", totalStations);
    } else {
      Serial.println("JSON Parsing failed.");
    }
  } else {
    Serial.printf("Failed to fetch JSON. HTTP Error Code: %d\n", httpCode);
  }
  http.end();
}

void saveLastPlayedStation() {
  if (totalStations <= 0 || currentStationIdx < 0 || currentStationIdx >= totalStations) {
    return;
  }
  if (!preferences.begin("radio", false)) {
    Serial.println("Failed to open preferences for writing.");
    return;
  }
  preferences.putInt("lastIdx", currentStationIdx);
  preferences.putString("lastUrl", playlist[currentStationIdx].url);
  preferences.end();
  Serial.printf("Saved last played station idx=%d\n", currentStationIdx);
}

void loadLastPlayedStation() {
  if (totalStations <= 0) {
    currentStationIdx = 0;
    return;
  }

  if (!preferences.begin("radio", true)) {
    Serial.println("No saved last station; starting at index 0.");
    currentStationIdx = 0;
    return;
  }

  String lastUrl = preferences.getString("lastUrl", "");
  int lastIdx = preferences.getInt("lastIdx", 0);
  preferences.end();

  // Prefer matching by URL so playlist reordering still resumes the same station
  if (lastUrl.length() > 0) {
    for (int i = 0; i < totalStations; i++) {
      if (playlist[i].url == lastUrl) {
        currentStationIdx = i;
        Serial.printf("Restored last played station by URL idx=%d\n", currentStationIdx);
        return;
      }
    }
  }

  if (lastIdx >= 0 && lastIdx < totalStations) {
    currentStationIdx = lastIdx;
  } else {
    currentStationIdx = 0;
  }
  Serial.printf("Restored last played station idx=%d\n", currentStationIdx);
}

bool initAudioHardware() {
  pinMode(AUDIO_AMP_EN, OUTPUT);
  digitalWrite(AUDIO_AMP_EN, LOW);

  Wire.begin(TOUCH_SDA, TOUCH_SCL, AUDIO_I2C_HZ);

  es8311I2S.setPins(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!es8311I2S.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Failed to initialize ES8311 I2S bus.");
    return false;
  }

  if (es8311_codec_init() != ESP_OK) {
    Serial.println("ES8311 codec init failed.");
    return false;
  }

  return true;
}

void drawBaseUI() {
  lcd.fillScreen(TFT_BLACK);
  lcd.fillRect(0, 0, 320, 45, lgfx::color565(30, 30, 30));
  lcd.setTextColor(TFT_GOLD);
  lcd.setTextSize(1.5);
  lcd.drawString("TC RADIOS | Personal Dashboard", 15, 15);
  
  lcd.fillRect(20, 170, 70, 45, lgfx::color565(0, 100, 200));  
  lcd.fillRect(125, 170, 70, 45, lgfx::color565(0, 180, 80)); 
  lcd.fillRect(230, 170, 70, 45, lgfx::color565(0, 100, 200)); 
  
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1.5);
  lcd.drawString("PREV", 38, 185);
  lcd.drawString("PAUSE", 140, 185);
  lcd.drawString("NEXT", 250, 185);
}

void updateDisplayStrings() {
  lcd.fillRect(10, 60, 300, 30, TFT_BLACK);
  lcd.setTextColor(TFT_CYAN);
  lcd.setTextSize(2);
  if (totalStations > 0) {
    lcd.drawString(playlist[currentStationIdx].name.substring(0, 24), 15, 60);
  } else {
    lcd.drawString("No Stations Loaded", 15, 60);
  }

  lcd.fillRect(10, 100, 300, 50, TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(1.5);
  lcd.drawString(currentTrack.substring(0, 35), 15, 105);
}

void audio_showstreamtitle(const char *info){
    currentTrack = String(info);
}
