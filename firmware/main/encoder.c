#include "encoder.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "mxcam_display.h"
#include "mxcam_link.h"
#include "mxcam_proto.h"
#include "pushq.h"

static const char *TAG = "encoder";

#define FRAME_BYTES ((size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2)
#define OUT_CAP (65535u - MXP_FRAME_HEADER_SIZE) /* room to see how big an oversize JPEG got */
#define MAX_JPEG (PUSHQ_MAX_FRAME_PAYLOAD - MXP_FRAME_HEADER_SIZE) /* the link's FRAME cap */
#define REENC_STEP 20
#define MIN_QUALITY 10
#define ENC_TASK_STACK 8192
#define ENC_TASK_PRIO 3 /* pipeline is 5 (core 1), link TX 4 and RX 6 */

static uint8_t *s_stage;   /* 16-byte aligned RGB565 copy of the frame (PSRAM) */
static uint8_t *s_payload; /* FRAME header + JPEG */
static jpeg_enc_handle_t s_enc;
static uint8_t s_enc_quality; /* quality the encoder handle is set to */
static TaskHandle_t s_task;
static atomic_bool s_busy;
static uint32_t s_frame_id, s_ts_ms;
static uint8_t s_quality;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_frames, s_send_fail, s_enc_fail, s_reenc, s_too_big;
static uint32_t s_enc_sum_ms, s_enc_max_ms;
static uint64_t s_jpeg_sum;

/* Encodes s_stage at quality q into s_payload after the header. Returns the
 * JPEG size, 0 on error. */
static int encode(uint8_t q)
{
    if (q != s_enc_quality) {
        if (jpeg_enc_set_quality(s_enc, q) != JPEG_ERR_OK) return 0;
        s_enc_quality = q;
    }
    int out = 0;
    jpeg_error_t e = jpeg_enc_process(s_enc, s_stage, (int)FRAME_BYTES, s_payload + MXP_FRAME_HEADER_SIZE,
                                      (int)OUT_CAP, &out);
    return e == JPEG_ERR_OK ? out : 0;
}

static void encoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        uint8_t q = s_quality;
        bool reenc = false;
        int jlen = encode(q);
        /* Over the push cap (or failed, e.g. out of output room): one retry at lower quality. */
        if ((jlen == 0 || jlen > (int)MAX_JPEG) && q > MIN_QUALITY) {
            q = q > MIN_QUALITY + REENC_STEP ? (uint8_t)(q - REENC_STEP) : MIN_QUALITY;
            jlen = encode(q);
            reenc = true;
        }
        uint32_t enc_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        bool failed = jlen == 0, big = jlen > (int)MAX_JPEG, sent = false;
        if (!failed && !big) {
            mxp_frame_header_t h = { s_frame_id, s_ts_ms, MXCAM_LCD_W, MXCAM_LCD_H };
            mxp_pack_frame_header(&h, s_payload);
            /* Non-blocking: the link TX task owns the port and the push seq. */
            sent = mxcam_link_push(MXP_CMD_PUSH_FRAME, s_payload, (uint16_t)(jlen + MXP_FRAME_HEADER_SIZE));
        }

        taskENTER_CRITICAL(&s_stats_lock);
        if (reenc) s_reenc++;
        if (failed) {
            s_enc_fail++;
        } else if (big) {
            s_too_big++;
        } else {
            s_enc_sum_ms += enc_ms;
            if (enc_ms > s_enc_max_ms) s_enc_max_ms = enc_ms;
            s_jpeg_sum += (uint32_t)jlen;
            if (sent) s_frames++; else s_send_fail++;
        }
        taskEXIT_CRITICAL(&s_stats_lock);

        atomic_store(&s_busy, false);
    }
}

esp_err_t encoder_start(void)
{
    /* esp_new_jpeg needs a 16-byte aligned input on the S3; its allocator
     * prefers PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT). */
    s_stage = jpeg_calloc_align(FRAME_BYTES, 16);
    s_payload = heap_caps_malloc(OUT_CAP + MXP_FRAME_HEADER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stage || !s_payload) {
        ESP_LOGE(TAG, "buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = MXCAM_LCD_W;
    cfg.height = MXCAM_LCD_H;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_BE; /* camera byte order: high byte first */
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = 60;
    cfg.task_enable = false; /* encode on this task only (core 0) */
    jpeg_error_t je = jpeg_enc_open(&cfg, &s_enc);
    if (je != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open failed: %d", (int)je);
        return ESP_FAIL;
    }
    s_enc_quality = cfg.quality;
    atomic_init(&s_busy, false);
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", ENC_TASK_STACK, NULL, ENC_TASK_PRIO, &s_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool encoder_try_submit(const uint16_t *rgb565, uint32_t frame_id, uint32_t timestamp_ms, uint8_t quality)
{
    if (!s_task) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) return false;
    memcpy(s_stage, rgb565, FRAME_BYTES);
    s_frame_id = frame_id;
    s_ts_ms = timestamp_ms;
    s_quality = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    xTaskNotifyGive(s_task);
    return true;
}

void encoder_take_stats(encoder_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_lock);
    unsigned done = s_frames + s_send_fail; /* frames that produced a pushable JPEG */
    out->frames = s_frames;
    out->send_fail = s_send_fail;
    out->enc_fail = s_enc_fail;
    out->reenc = s_reenc;
    out->too_big = s_too_big;
    out->enc_avg_ms = done ? s_enc_sum_ms / done : 0;
    out->enc_max_ms = s_enc_max_ms;
    out->jpeg_avg = done ? (uint32_t)(s_jpeg_sum / done) : 0;
    s_frames = s_send_fail = s_enc_fail = s_reenc = s_too_big = 0;
    s_enc_sum_ms = s_enc_max_ms = 0;
    s_jpeg_sum = 0;
    taskEXIT_CRITICAL(&s_stats_lock);
    out->stack_hw = s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}
