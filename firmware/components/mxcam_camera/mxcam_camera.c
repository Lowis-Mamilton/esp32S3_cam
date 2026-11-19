#include "mxcam_camera.h"

#include "esp_log.h"

static const char *TAG = "mxcam_cam";

/* Pins per docs/SPEC.md section 4; no PWDN/RESET wired. */
#define PIN_XCLK  15
#define PIN_SIOD  4
#define PIN_SIOC  5
#define PIN_VSYNC 6
#define PIN_HREF  7
#define PIN_PCLK  13

esp_err_t mxcam_camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD,
        .pin_sccb_scl = PIN_SIOC,
        .pin_d0 = 11, .pin_d1 = 9,  .pin_d2 = 8,  .pin_d3 = 10,
        .pin_d4 = 12, .pin_d5 = 18, .pin_d6 = 17, .pin_d7 = 16,
        .pin_vsync = PIN_VSYNC,
        .pin_href = PIN_HREF,
        .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = 20 * 1000 * 1000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = -1,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", (unsigned)err);
        return err;
    }
    /* Recognition needs the TRUE (non-mirrored) image: a mirrored AprilTag
     * does not decode (ADR-0010). The sensor's default output is mirrored on
     * this board, so turn the sensor's horizontal mirror on; the TFT mirrors
     * the picture back for the user (mxcam_display). */
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->set_hmirror) {
        int before = s->status.hmirror, vflip = s->status.vflip;
        s->set_hmirror(s, 1);
        ESP_LOGI(TAG, "sensor hmirror %d -> 1 (vflip %d)", before, vflip);
    }
    return ESP_OK;
}

camera_fb_t *mxcam_camera_grab(void)
{
    return esp_camera_fb_get();
}

void mxcam_camera_release(camera_fb_t *fb)
{
    if (fb) esp_camera_fb_return(fb);
}
