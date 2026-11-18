#include "encoder.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "mxcam_display.h"
#include "mxcam_link.h"
#include "mxcam_proto.h"

static const char *TAG = "encoder";

#define FRAME_BYTES ((size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2)
#define MAX_PAYLOAD 65535u /* protocol limit for one packet's payload */
#define ENC_TASK_STACK 8192
#define ENC_TASK_PRIO 3 /* pipeline is 5, link RX is 6 */

static uint8_t *s_stage;   /* RGB565 copy of the frame to encode */
static uint8_t *s_payload; /* FRAME header + JPEG */
static uint8_t *s_tx;      /* encoded packet */
static TaskHandle_t s_task;
static atomic_bool s_busy;
static uint32_t s_frame_id, s_ts_ms;
static uint8_t s_quality;
static uint8_t s_push_seq;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_frames, s_send_fail, s_enc_fail;
static uint32_t s_enc_sum_ms, s_enc_max_ms;
static uint64_t s_jpeg_sum;

static void encoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        uint8_t *jpg = NULL;
        size_t jlen = 0;
        bool ok = fmt2jpg(s_stage, FRAME_BYTES, MXCAM_LCD_W, MXCAM_LCD_H, PIXFORMAT_RGB565,
                          s_quality, &jpg, &jlen);
        uint32_t enc_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        bool sent = false, enc_failed = false;
        if (!ok || !jpg || jlen == 0 || jlen + MXP_FRAME_HEADER_SIZE > MAX_PAYLOAD) {
            enc_failed = true;
        } else {
            mxp_frame_header_t h = { s_frame_id, s_ts_ms, MXCAM_LCD_W, MXCAM_LCD_H };
            mxp_pack_frame_header(&h, s_payload);
            memcpy(s_payload + MXP_FRAME_HEADER_SIZE, jpg, jlen);
            size_t n = mxp_encode(MXP_CMD_PUSH_FRAME, s_push_seq++, s_payload,
                                  (uint16_t)(jlen + MXP_FRAME_HEADER_SIZE), s_tx, MAX_PAYLOAD + MXP_OVERHEAD);
            sent = n > 0 && mxcam_link_send(s_tx, n);
        }
        free(jpg); /* fmt2jpg(NULL) is fine too; free(NULL) is a no-op */

        taskENTER_CRITICAL(&s_stats_lock);
        if (enc_failed) {
            s_enc_fail++;
        } else {
            s_enc_sum_ms += enc_ms;
            if (enc_ms > s_enc_max_ms) s_enc_max_ms = enc_ms;
            s_jpeg_sum += jlen;
            if (sent) s_frames++; else s_send_fail++;
        }
        taskEXIT_CRITICAL(&s_stats_lock);

        atomic_store(&s_busy, false);
    }
}

esp_err_t encoder_start(void)
{
    s_stage = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_payload = heap_caps_malloc(MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx = heap_caps_malloc(MAX_PAYLOAD + MXP_OVERHEAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stage || !s_payload || !s_tx) {
        ESP_LOGE(TAG, "PSRAM buffers allocation failed");
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&s_busy, false);
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", ENC_TASK_STACK, NULL, ENC_TASK_PRIO, &s_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool encoder_try_submit(const uint16_t *rgb565, uint32_t frame_id, uint32_t timestamp_ms, uint8_t quality)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) return false;
    memcpy(s_stage, rgb565, FRAME_BYTES);
    s_frame_id = frame_id;
    s_ts_ms = timestamp_ms;
    s_quality = quality;
    xTaskNotifyGive(s_task);
    return true;
}

void encoder_take_stats(encoder_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_lock);
    unsigned done = s_frames + s_send_fail; /* frames that reached JPEG output */
    out->frames = s_frames;
    out->send_fail = s_send_fail;
    out->enc_fail = s_enc_fail;
    out->enc_avg_ms = done ? s_enc_sum_ms / done : 0;
    out->enc_max_ms = s_enc_max_ms;
    out->jpeg_avg = done ? (uint32_t)(s_jpeg_sum / done) : 0;
    s_frames = s_send_fail = s_enc_fail = 0;
    s_enc_sum_ms = s_enc_max_ms = 0;
    s_jpeg_sum = 0;
    taskEXIT_CRITICAL(&s_stats_lock);
}
