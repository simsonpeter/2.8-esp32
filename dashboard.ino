#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <ArduinoJson.h>

// --- HARDWARE CONFIGURATION (ES3C28P 2.8") ---
#define I2S_DOUT      GPIO_NUM_1  
#define I2S_BCLK      GPIO_NUM_2  
#define I2S_LRCK      GPIO_NUM_4  

#define MAX_STATIONS 20

struct RadioStation {
  String name;
  String url;
};

RadioStation playlist[MAX_STATIONS];
int totalStations = 0;
int currentStationIdx = 0;

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
      cfg.pin_mosi = 11; cfg.pin_miso = 13; cfg.pin_sclk = 12; cfg.pin_dc = 46;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 10; cfg.pin_rst = -1;
      cfg.panel_width = 240; cfg.panel_height = 320;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 45;                  
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0; cfg.x_max = 239; cfg.y_min = 0; cfg.y_max = 319;
      cfg.pin_sda = 16; cfg.pin_scl = 15; cfg.pin_int = 17; cfg.pin_rst = 18;
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

String currentTrack = "Connecting...";
bool isPlaying = true;

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

  if (totalStations > 0) {
    currentTrack = "Connecting to audio stream...";
  } else {
    currentTrack = "Error: Playlist empty";
  }
  updateDisplayStrings();

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(12); 
  
  if (totalStations > 0) {
    audio.connecttohost(playlist[currentStationIdx].url.c_str());
  }
}

void loop() {
  audio.loop();

  int32_t touchX, touchY;
  if (lcd.getTouch(&touchX, &touchY)) {
    if ((millis() - lastDebounceTime) > debounceDelay) {
      if (touchY > 160 && touchY < 220 && touchX > 20 && touchX < 90) {
        if (totalStations > 0) {
          currentStationIdx = (currentStationIdx - 1 + totalStations) % totalStations;
          audio.connecttohost(playlist[currentStationIdx].url.c_str());
          currentTrack = "Loading stream...";
          updateDisplayStrings();
          lastDebounceTime = millis();
        }
      }
      else if (touchY > 160 && touchY < 220 && touchX > 120 && touchX < 190) {
        isPlaying = !isPlaying;
        audio.pauseResume(); 
        lastDebounceTime = millis();
      }
      else if (touchY > 160 && touchY < 220 && touchX > 220 && touchX < 290) {
        if (totalStations > 0) {
          currentStationIdx = (currentStationIdx + 1) % totalStations;
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
