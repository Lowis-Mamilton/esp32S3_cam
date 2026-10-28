#include <Arduino.h>
#include <TFT_eSPI.h>
#include "esp_camera.h"
#include "img_converters.h"

TFT_eSPI tft = TFT_eSPI();

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      15
#define SIOD_GPIO_NUM       4
#define SIOC_GPIO_NUM       5
#define Y9_GPIO_NUM        16
#define Y8_GPIO_NUM        17
#define Y7_GPIO_NUM        18
#define Y6_GPIO_NUM        12
#define Y5_GPIO_NUM        10
#define Y4_GPIO_NUM         8
#define Y3_GPIO_NUM         9
#define Y2_GPIO_NUM        11
#define VSYNC_GPIO_NUM      6
#define HREF_GPIO_NUM       7
#define PCLK_GPIO_NUM      13

const int IMG_W = 240;
const int IMG_H = 240;
const int DISPLAY_Y = 40;          // (320-240)/2 → 上下黑邊
const int SEND_INTERVAL_MS = 250;
const int JPEG_QUALITY = 15;

void setup() {
  Serial.begin(921600);
  delay(1500);

  tft.init();
  tft.setRotation(0);
  tft.setSwapBytes(false);         // 跟你正常預覽一樣
  tft.fillScreen(TFT_BLACK);

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
  config.frame_size   = FRAMESIZE_240X240;  // 對齊 240 寬面板
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.fb_count     = 2;
  config.jpeg_quality = 12;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    tft.fillScreen(TFT_RED);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
  }

  Serial.println("USB_CAM_READY");
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  // TFT：240x240，上下黑邊
  tft.pushImage(0, DISPLAY_Y, IMG_W, IMG_H, (uint16_t *)fb->buf);

  static unsigned long lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, JPEG_QUALITY, &jpg_buf, &jpg_len) && jpg_buf && jpg_len > 0) {
      uint32_t len = (uint32_t)jpg_len;
      Serial.write(0xAA);
      Serial.write(0x55);
      Serial.write((uint8_t)(len & 0xFF));
      Serial.write((uint8_t)((len >> 8) & 0xFF));
      Serial.write((uint8_t)((len >> 16) & 0xFF));
      Serial.write((uint8_t)((len >> 24) & 0xFF));
      Serial.write(jpg_buf, jpg_len);
      Serial.flush();
      free(jpg_buf);
    }
  }

  esp_camera_fb_return(fb);
}