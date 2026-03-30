// Internet Radio for Waveshare ESP32-S3-Touch-LCD-1.54
// Original author: VolosR (Volos Projects 2026)
// https://github.com/VolosR/WaveshareRadioStream
// Cleaned up and refactored by rdmcnabb

#include "Arduino.h"
#include "WiFiMulti.h"
#include "Audio.h"
#include "SD_MMC.h"
#include "FS.h"
#include <Arduino_GFX_Library.h>
#include <LovyanGFX.hpp>
#include "es8311.h"
#include "esp_check.h"
#include "Wire.h"
#include "NotoSansBold15.h"

// --- Pin Definitions ---
#define PA_CTRL   7
#define I2S_MCLK  8
#define I2S_BCLK  9
#define I2S_LRC   10
#define I2S_DOUT  12
#define I2C_SDA   42
#define I2C_SCL   41
#define GFX_BL    46

#define BTN_SLEEP   0
#define BTN_STATION 5
#define BTN_VOLUME  4
#define BATT_ADC    1
#define BATT_ENABLE 2

// --- Audio Codec Config ---
#define SAMPLE_RATE     16000
#define MCLK_MULTIPLE   256
#define MCLK_FREQ_HZ    (SAMPLE_RATE * MCLK_MULTIPLE)
#define CODEC_VOLUME    75

// --- Color Palette (RGB565) ---
#define COL_BLACK   0x0000
#define COL_YELLOW  0xFFE0
#define COL_ORANGE  0xFD20

// --- Display Setup ---
Arduino_DataBus *bus = new Arduino_ESP32SPI(45, 21, 38, 39, -1);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 40, 0, true, 240, 240);

// --- Sprites ---
LGFX_Sprite sprite;
LGFX_Sprite spriteMarquee;

// --- Radio Stations ---
#define NUM_STATIONS 6
String stations[NUM_STATIONS] = {
  "https://discodiamond.radioca.st/autodj",
  "https://listen.radioking.com/radio/175279/stream/216784",
  "http://sc6.radiocaroline.net:8040/stream",
  "https://club-high.rautemusik.fm/;",
  "http://greece-media.monroe.edu/wgmc.mp3",
  "https://audio.radio-banovina.hr:9998/;"
};

// --- State ---
Audio audio;
WiFiMulti wifiMulti;
String ssid     = "YOUR_SSID";       // <-- Edit with your WiFi SSID
String password  = "YOUR_PASSWORD";  // <-- Edit with your WiFi password

String songPlaying = "";
long   bitrate     = 0;
bool   connected   = false;
int    songScrollX = -220;
float  voltage     = 4.20;
int    batLevel    = 0;
int    chosen      = 0;
int    volume      = 2;
int    rssi        = 0;
bool   canDraw     = false;
bool   debStation  = false;
bool   debVolume   = false;

// RSSI history for real signal graph
#define GRAPH_BARS 12
int rssiHistory[GRAPH_BARS] = {0};
int rssiHistIdx = 0;

// UI color palette
unsigned short grays[18];
unsigned short gray;
unsigned short light;
String btnLabels[3] = {"P", "S", "V"};

// --- ES8311 Codec Init ---
static esp_err_t es8311_codec_init(void) {
  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

  const es8311_clock_config_t es_clk = {
    .mclk_inverted     = false,
    .sclk_inverted     = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency    = MCLK_FREQ_HZ,
    .sample_frequency  = SAMPLE_RATE
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, MCLK_FREQ_HZ, SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, CODEC_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");

  return ESP_OK;
}

// --- Battery Measurement ---
void measureBatt() {
  uint16_t mv = analogReadMilliVolts(BATT_ADC);
  float vbat = (mv / 1000.0) * 3.0;
  voltage = vbat;

  float pct = (vbat - 3.0) / (4.2 - 3.0);
  pct = constrain(pct, 0.0, 1.0);
  batLevel = pct * 13.0;
}

// --- RSSI to graph bar height (0-4) ---
int rssiToBar(int rssiVal) {
  // Typical range: -90 (weak) to -30 (strong)
  if (rssiVal >= -40) return 4;
  if (rssiVal >= -55) return 3;
  if (rssiVal >= -70) return 2;
  if (rssiVal >= -85) return 1;
  return 0;
}

// --- Draw Song Marquee ---
void drawMarquee() {
  songScrollX--;
  if (songScrollX < -220) songScrollX = 220;

  spriteMarquee.fillSprite(TFT_BLACK);
  spriteMarquee.drawString(songPlaying, songScrollX, 5);

  uint16_t *buf = (uint16_t *)spriteMarquee.getBuffer();
  int total = 230 * 16;
  for (int i = 0; i < total; i++) {
    buf[i] = __builtin_bswap16(buf[i]);
  }
  gfx->draw16bitRGBBitmap(5, 213, buf, 230, 16);
}

// --- Draw Main UI ---
void drawUI() {
  gray  = grays[16];
  light = grays[12];

  // Background
  sprite.fillRect(0, 0, 240, 240, gray);

  // Stations panel
  sprite.fillRect(4, 20, 150, 172, COL_BLACK);
  sprite.drawRect(4, 20, 150, 172, light);

  // WiFi/graph panel
  sprite.fillRect(160, 20, 74, 60, COL_BLACK);
  sprite.drawRect(160, 20, 74, 60, light);
  sprite.fillRect(174, 24, 5, 10, TFT_RED);
  sprite.fillRect(174, 37, 5, 10, TFT_GREEN);

  // Battery icon
  sprite.drawRect(210, 36, 17, 10, TFT_GREEN);
  sprite.fillRect(212, 38, batLevel, 6, TFT_GREEN);
  sprite.fillRect(227, 39, 2, 4, TFT_GREEN);

  // Bitrate panel
  sprite.fillRect(160, 176, 74, 16, COL_BLACK);
  sprite.drawRect(160, 176, 74, 16, light);

  // Volume slider
  sprite.fillRoundRect(160, 140, 74, 3, 2, COL_YELLOW);
  sprite.fillRoundRect(146 + (volume * 15), 137, 14, 8, 2, grays[2]);
  sprite.fillRoundRect(149 + (volume * 15), 139, 8, 4, 2, grays[10]);

  // Song playing panel
  sprite.fillRect(4, 212, 232, 18, COL_BLACK);
  sprite.drawRect(4, 212, 232, 18, light);

  // Divider bar between stations and right panel
  sprite.fillRect(149, 20, 5, 172, grays[11]);
  int sliderPos = 12;
  sprite.fillRect(149, sliderPos + 8, 5, 20, grays[2]);
  sprite.fillRect(151, sliderPos + 12, 1, 12, grays[16]);

  // Accent lines
  sprite.fillRect(4, 7, 150, 3, COL_ORANGE);
  sprite.fillRect(190, 5, 45, 3, COL_ORANGE);
  sprite.fillRect(160, 194, 74, 1, COL_ORANGE);
  sprite.fillRect(190, 11, 45, 3, grays[6]);

  // Border
  sprite.drawRect(0, 0, 239, 239, light);
  sprite.fillRect(5, 234, 230, 2, grays[13]);

  // Headers
  sprite.setTextColor(grays[1], gray);
  sprite.drawString(" STATIONS ", 42, 2, 2);
  sprite.drawString("WEB", 160, 2, 2);

  // Station list
  for (int i = 0; i < NUM_STATIONS; i++) {
    if (i == chosen)
      sprite.setTextColor(TFT_GREEN, TFT_BLACK);
    else
      sprite.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    sprite.drawString(stations[i].substring(0, 20), 10, 26 + (i * 19), 2);
  }

  // Labels
  sprite.setTextColor(grays[0], gray);
  sprite.drawString("INTERNET", 160, 86);
  sprite.setTextColor(TFT_RED, gray);
  sprite.drawString("RADIO", 160, 102);

  sprite.setTextColor(grays[6], gray);
  sprite.drawString("SONG PLAYING", 6, 200, 1);
  sprite.drawString("VOLUME", 160, 124, 1);

  // WiFi vertical text
  sprite.setTextColor(grays[10], TFT_BLACK);
  sprite.drawString("W", 165, 24, 1);
  sprite.drawString("I", 165, 34, 1);
  sprite.drawString("F", 165, 44, 1);
  sprite.drawString("I", 165, 54, 1);

  // Dynamic data
  sprite.setTextColor(TFT_GREEN, TFT_BLACK);
  sprite.drawString("BITRATE " + String(bitrate), 164, 180, 1);
  sprite.drawString("RSSI:" + String(rssi), 183, 24, 1);
  sprite.drawString(String(voltage), 183, 37, 1);

  sprite.setTextColor(grays[11], gray);
  sprite.drawString("VOLOS PROJECTS 2026", 122, 200, 1);

  // Signal graph — real RSSI history
  for (int i = 0; i < GRAPH_BARS; i++) {
    int idx = (rssiHistIdx + i) % GRAPH_BARS;
    for (int j = 0; j < rssiHistory[idx]; j++)
      sprite.fillRect(172 + (i * 5), 71 - j * 4, 4, 3, grays[4]);
  }

  // Button labels
  sprite.setTextColor(grays[16], grays[5]);
  for (int i = 0; i < 3; i++) {
    sprite.fillRoundRect(160 + (i * 26), 152, 22, 18, 4, grays[5]);
    sprite.drawString(btnLabels[i], 166 + (i * 26), 154);
  }

  // Push sprite to display (byte-swap for endianness mismatch between LovyanGFX and Arduino_GFX)
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  int total = 240 * 240;
  for (int i = 0; i < total; i++) {
    buf[i] = __builtin_bswap16(buf[i]);
  }
  gfx->draw16bitRGBBitmap(0, 0, buf, 240, 240);

  canDraw = false;
  drawMarquee();
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  // Battery measurement pin
  gpio_hold_dis((gpio_num_t)BATT_ENABLE);
  pinMode(BATT_ENABLE, OUTPUT);
  digitalWrite(BATT_ENABLE, HIGH);

  // Buttons
  pinMode(BTN_SLEEP, INPUT_PULLUP);
  pinMode(BTN_STATION, INPUT_PULLUP);
  pinMode(BTN_VOLUME, INPUT_PULLUP);

  // Audio amplifier + codec
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  es8311_codec_init();
  gpio_hold_en((gpio_num_t)BATT_ENABLE);

  // Display init
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  analogWrite(GFX_BL, 110);

  gfx->setCursor(2, 20);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_GREEN);
  gfx->println("connecting to WI-FI");

  // Sprite setup
  sprite.setColorDepth(16);
  sprite.createSprite(240, 240);
  spriteMarquee.createSprite(230, 16);
  sprite.loadFont(NotoSansBold15);

  // Build gray palette
  int co = 214;
  for (int i = 0; i < 18; i++) {
    grays[i] = sprite.color565(co, co, co + 40);
    co -= 13;
  }
  spriteMarquee.setTextColor(grays[0], TFT_BLACK);

  // WiFi connect with retries
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(ssid.c_str(), password.c_str());

  int retries = 0;
  while (wifiMulti.run() != WL_CONNECTED && retries < 10) {
    delay(1000);
    retries++;
    Serial.printf("WiFi attempt %d/10...\n", retries);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected to %s, IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connection failed after 10 attempts");
  }

  // Audio init
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume * 4);
  audio.connecttohost(stations[0].c_str());
}

// --- Main Loop ---
void loop() {
  static unsigned long lastUpdate = 0;
  static unsigned long lastSlide  = 0;

  // Periodic update: RSSI, battery, redraw (~4x/sec)
  if (millis() - lastUpdate > 240) {
    lastUpdate = millis();
    rssi = WiFi.RSSI();
    measureBatt();

    // Record real RSSI into history ring buffer
    rssiHistory[rssiHistIdx] = connected ? rssiToBar(rssi) : 0;
    rssiHistIdx = (rssiHistIdx + 1) % GRAPH_BARS;

    canDraw = true;

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    } else {
      connected = false;
      songPlaying = "WIFI NOT CONNECTED";
    }
  }

  // Song title marquee scroll (~33fps)
  if (millis() - lastSlide > 30) {
    lastSlide = millis();
    drawMarquee();
  }

  // Button: station select
  if (digitalRead(BTN_STATION) == LOW) {
    if (!debStation) {
      debStation = true;
      chosen = (chosen + 1) % NUM_STATIONS;
      audio.connecttohost(stations[chosen].c_str());
      canDraw = true;
    }
  } else {
    debStation = false;
  }

  // Button: volume cycle
  if (digitalRead(BTN_VOLUME) == LOW) {
    if (!debVolume) {
      debVolume = true;
      volume = (volume % 5) + 1;
      audio.setVolume(volume * 4);
      canDraw = true;
    }
  } else {
    debVolume = false;
  }

  // Button: deep sleep
  if (digitalRead(BTN_SLEEP) == LOW) {
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    digitalWrite(PA_CTRL, LOW);
    delay(200);
    esp_deep_sleep_start();
  }

  vTaskDelay(1);
  audio.loop();

  if (canDraw)
    drawUI();
}

// --- Audio Callbacks ---
void audio_info(const char *info) {
  Serial.print("info        ");
  Serial.println(info);
}

void audio_id3data(const char *info) {
  Serial.print("id3data     ");
  Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.printf("station     %s\n", info);
  canDraw = true;
}

void audio_showstreamtitle(const char *info) {
  Serial.printf("streamtitle %s\n", info);
  songPlaying = info;
  canDraw = true;
}

void audio_bitrate(const char *info) {
  Serial.printf("bitrate     %s\n", info);
  bitrate = String(info).toInt() / 1000;
}
