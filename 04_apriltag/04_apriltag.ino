#include <Arduino.h>
#include <TFT_eSPI.h>
#include "esp_camera.h"
#include "img_converters.h"

TFT_eSPI tft = TFT_eSPI();

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

const int SEND_INTERVAL_MS = 300;
const int JPEG_QUALITY = 10;

struct {
  bool valid = false;
  int id = 0;
  int cx = 0, cy = 0;
  int x0, y0, x1, y1, x2, y2, x3, y3;
  float rot = 0;
} tag;

String rxLine;
int rxCount = 0;

void parseTagLine(const String &line) {
  if (!line.startsWith("#TAG,")) return;
  rxCount++;

  if (line.indexOf("NONE") >= 0) {
    tag.valid = false;
    return;
  }

  int vals[11];
  int start = 5;
  for (int i = 0; i < 11; i++) {
    int comma = line.indexOf(',', start);
    if (comma < 0) return;
    vals[i] = line.substring(start, comma).toInt();
    start = comma + 1;
  }
  float rot = line.substring(start).toFloat();

  tag.valid = true;
  tag.id = vals[0];
  tag.cx = vals[1];
  tag.cy = vals[2];
  tag.x0 = vals[3]; tag.y0 = vals[4];
  tag.x1 = vals[5]; tag.y1 = vals[6];
  tag.x2 = vals[7]; tag.y2 = vals[8];
  tag.x3 = vals[9]; tag.y3 = vals[10];
  tag.rot = rot;
}

void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLine.length() > 4) parseTagLine(rxLine);
      rxLine = "";
    } else {
      rxLine += c;
      if (rxLine.length() > 160) rxLine = "";
    }
  }
}

void setup() {
  Serial.begin(921600);
  delay(1500);

  tft.init();
  tft.setRotation(0);
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_BLACK);

  // 開機就畫字，確認 TFT 活著
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(4, 8);
  tft.print("BOOT OK");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_QVGA;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.fb_count     = 2;
  config.jpeg_quality = 12;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&config) != ESP_OK) {
    tft.fillScreen(TFT_RED);
    tft.setCursor(4, 8);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.print("CAM FAIL");
    while (1) delay(1000);
  }
}

void loop() {
  pollSerial();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  // 畫面
  tft.pushImage(0, 40, 320, 240, (uint16_t *)fb->buf);

  // ===== 每一幀都重畫上方狀態（一定看得到）=====
  tft.fillRect(0, 0, 240, 40, TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.printf("rx:%d ", rxCount);
  if (tag.valid) {
    tft.printf("ID:%d %.0f", tag.id, tag.rot);
    // 框 + 中心（y 要 +40）
    uint16_t g = TFT_GREEN;
// 畫框時
    auto mx = [](int x) { return 319 - x; };

    tft.drawLine(mx(tag.x0), 40 + tag.y0, mx(tag.x1), 40 + tag.y1, g);
    tft.drawLine(mx(tag.x1), 40 + tag.y1, mx(tag.x2), 40 + tag.y2, g);
    tft.drawLine(mx(tag.x2), 40 + tag.y2, mx(tag.x3), 40 + tag.y3, g);
    tft.drawLine(mx(tag.x3), 40 + tag.y3, mx(tag.x0), 40 + tag.y0, g);
    tft.fillCircle(mx(tag.cx), 40 + tag.cy, 4, TFT_RED);
  } else {
    tft.print("WAIT PC");
  }

  // USB 傳圖給 Python
  static unsigned long lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, JPEG_QUALITY, &jpg_buf, &jpg_len) && jpg_buf) {
      uint32_t len = (uint32_t)jpg_len;
      Serial.write(0xAA);
      Serial.write(0x55);
      Serial.write((uint8_t)(len));
      Serial.write((uint8_t)(len >> 8));
      Serial.write((uint8_t)(len >> 16));
      Serial.write((uint8_t)(len >> 24));
      Serial.write(jpg_buf, jpg_len);
      Serial.flush();
      free(jpg_buf);
    }
  }

  esp_camera_fb_return(fb);
}