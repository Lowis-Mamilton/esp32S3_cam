#include "pipeline.h"

#include <stdio.h>
#include <string.h>

#include "encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_camera.h"
#include "mxcam_color.h"
#include "mxcam_display.h"
#include "mxcam_link.h"
#include "mxcam_overlay.h"
#include "mxcam_store.h"
#include "mxcam_tag.h"
#include "status_led.h"

static const char *TAG = "pipeline";

/* AprilTag (upstream apriltag, quad threshold + decode) needs a deep stack;
 * colour detection keeps ~3 KB of HSV tables on it. The 1 Hz log prints the
 * high-water mark. */
#define PIPE_STACK (24 * 1024)
#define PIPE_PRIO 5
#define ICON_SIZE 12
#define ICON_MARGIN 4
#define BOX_THICKNESS 2

/* Studio's ID palette (pc-app/src/theme.css --id-1..--id-8) as RGB565 in the
 * panel's big-endian byte order. */
#define HEX565(rgb) ((uint16_t)(((((rgb) >> 19) & 0x1F) << 11) | ((((rgb) >> 10) & 0x3F) << 5) | (((rgb) >> 3) & 0x1F)))
#define ID_COLOR(rgb) MXO_RGB565(HEX565(rgb))
static const uint16_t k_id_colors[8] = {
    ID_COLOR(0xef4b4b), ID_COLOR(0xa3d62f), ID_COLOR(0x34d27b), ID_COLOR(0x2cc8d8),
    ID_COLOR(0x4d8ef5), ID_COLOR(0x8f6df2), ID_COLOR(0xd45be0), ID_COLOR(0xf2609b),
};

static const char *const k_fn_names[] = { "NONE", "COLOR", "TAG" };

static mxs_state_t *s_state;
static const char *s_boot_error;

/* Per-frame copy of what the pipeline needs from the shared state. */
typedef struct {
    uint32_t now_ms;
    bool connected, want_frames, want_results, streaming;
    uint8_t fps, quality, fn;
    mxp_config_t cfg;
    mxc_learned_t colors[MXST_MAX_COLORS];
    uint8_t n_colors;
} frame_ctx_t;

static void copy_colors(frame_ctx_t *c)
{
    c->n_colors = s_state->data->n_colors;
    memcpy(c->colors, s_state->data->colors, sizeof(c->colors));
}

/* Boxes + ASCII labels ("ID1" / "T3") above them (inside near the top edge
 * when there is no room above). Colour id 0 (not learned) is never drawn. */
static void draw_results(mxo_canvas_t *c, const mxp_results_t *res)
{
    for (int i = 0; i < res->count; i++) {
        const mxp_result_t *r = &res->r[i];
        uint16_t col;
        char label[8];
        if (res->function_id == MXP_FUNC_COLOR) {
            if (r->id < 1 || r->id > MXST_MAX_COLORS) continue;
            col = k_id_colors[r->id - 1];
            snprintf(label, sizeof(label), "ID%u", (unsigned)r->id);
        } else if (res->function_id == MXP_FUNC_APRILTAG) {
            col = k_id_colors[r->id % 7]; /* id % 7 + 1 -> palette index id % 7 */
            snprintf(label, sizeof(label), "T%u", (unsigned)r->id);
        } else {
            continue;
        }
        int x0 = r->x - r->w / 2, y0 = r->y - r->h / 2;
        mxo_draw_rect(c, x0, y0, r->w, r->h, BOX_THICKNESS, col);
        int tw = (int)strlen(label) * 8;
        int lx = x0;
        if (lx + tw > c->width) lx = c->width - tw;
        if (lx < 0) lx = 0;
        int ly = y0 - 9;
        if (ly < 0) ly = y0 + BOX_THICKNESS + 1;
        mxo_draw_text(c, lx, ly, label, MXO_BLACK, col);
    }
}

static void pipeline_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    int frames = 0, fps = 0;
    /* Cumulative error counters, logged once per second. */
    unsigned grab_fail = 0, bad_size = 0, lcd_err = 0, results_push_fail = 0;
    uint64_t proc_sum_us = 0;
    uint32_t proc_max_us = 0;
    uint8_t last_count = 0, last_fn = 0;
    uint32_t frame_id = 0;
    /* Streaming schedule: next_due_ms advances by one interval per submitted
     * frame (not "now + interval") so the average rate matches max_fps even
     * when the frame period does not divide the interval. */
    uint32_t next_due_ms = 0;
    bool was_wanting = false, streaming_now = false;
    static frame_ctx_t ctx;
    static mxp_results_t res;
    static uint8_t packed[MXS_RESULTS_MAX];
    uint8_t learn_out[32];

    for (;;) {
        camera_fb_t *fb = mxcam_camera_grab();
        if (!fb || fb->width != MXCAM_LCD_W || fb->height != MXCAM_LCD_H ||
            fb->len < (size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2) {
            /* mxcam_display_draw and the detectors assume a full 320x240 RGB565 buffer. */
            if (!fb) grab_fail++;
            else bad_size++;
            mxcam_camera_release(fb);
            /* Keep enforcing the stream timeout while the camera fails. */
            mxcam_link_state_lock();
            mxs_tick(s_state, (uint32_t)(esp_timer_get_time() / 1000));
            mxcam_link_state_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            const uint16_t *frame = (const uint16_t *)fb->buf;
            uint8_t lseq = 0;
            mxp_learn_req_t lreq;
            bool learn;

            mxcam_link_state_lock();
            /* Sample the clock inside the lock, like the link RX task does. */
            ctx.now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            mxs_tick(s_state, ctx.now_ms);
            ctx.connected = mxs_host_connected(s_state, ctx.now_ms);
            ctx.want_frames = mxs_wants_frames(s_state);
            ctx.want_results = mxs_wants_results(s_state);
            ctx.streaming = s_state->streaming;
            ctx.fps = mxs_effective_fps(s_state);
            ctx.quality = mxs_effective_quality(s_state);
            ctx.fn = s_state->function_id;
            ctx.cfg = s_state->data->cfg;
            copy_colors(&ctx);
            learn = mxs_take_pending_learn(s_state, &lseq, &lreq);
            mxcam_link_state_unlock();
            streaming_now = ctx.streaming;

            /* JPEG work happens on the encoder task (core 0). Here we only
             * copy the clean frame (before the overlay) when a push is due
             * and the encoder is idle; otherwise the frame is simply skipped. */
            if (ctx.want_frames) {
                if (!was_wanting) next_due_ms = ctx.now_ms;
                if ((int32_t)(ctx.now_ms - next_due_ms) >= 0 &&
                    encoder_try_submit(frame, frame_id, ctx.now_ms, ctx.quality)) {
                    uint32_t interval = 1000u / ctx.fps;
                    next_due_ms += interval;
                    if ((int32_t)(ctx.now_ms - next_due_ms) > (int32_t)interval) next_due_ms = ctx.now_ms;
                }
            }
            was_wanting = ctx.want_frames;

            int64_t tp = esp_timer_get_time();
            /* Deferred LEARN, claimed above: sample this frame. Every claimed
             * learn is completed (mxs_finish_learn returns 0 if it was
             * cancelled meanwhile, and then nothing is sent). */
            if (learn) {
                uint8_t rgb[3];
                bool ok = mxc_sample(frame, lreq.x, lreq.y, lreq.w, lreq.h, rgb);
                mxcam_link_state_lock();
                size_t n = mxs_finish_learn(s_state, lseq, ok ? rgb : NULL, learn_out, sizeof(learn_out));
                copy_colors(&ctx); /* detect the new colour on this very frame */
                mxcam_link_state_unlock();
                if (n > 0) mxcam_link_send(learn_out, n);
                if (ok)
                    ESP_LOGI(TAG, "learn seq=%u rgb=%u,%u,%u", lseq, rgb[0], rgb[1], rgb[2]);
                else
                    ESP_LOGI(TAG, "learn seq=%u: no colour", lseq);
            }

            memset(&res, 0, sizeof(res));
            res.frame_id = frame_id;
            res.function_id = ctx.fn;
            if (ctx.fn == MXP_FUNC_COLOR) {
                mxc_params_t p = { ctx.cfg.tolerance, ctx.cfg.min_area, ctx.cfg.color_max };
                res.count = mxc_detect(frame, ctx.colors, ctx.n_colors, &p, res.r, MXP_MAX_RESULTS);
            } else if (ctx.fn == MXP_FUNC_APRILTAG) {
                res.count = mxt_detect(frame, res.r, MXP_MAX_RESULTS, NULL);
            }
            uint32_t proc_us = (uint32_t)(esp_timer_get_time() - tp);
            proc_sum_us += proc_us;
            if (proc_us > proc_max_us) proc_max_us = proc_us;
            last_count = res.count;
            last_fn = ctx.fn;

            size_t plen = mxp_pack_results(&res, packed, sizeof(packed));
            if (plen > 0) {
                mxcam_link_state_lock();
                mxs_set_results(s_state, packed, (uint16_t)plen);
                mxcam_link_state_unlock();
                /* Non-blocking; RESULTS keeps only the latest. */
                if (ctx.want_results && !mxcam_link_push(MXP_CMD_PUSH_RESULTS, packed, (uint16_t)plen))
                    results_push_fail++;
            }
            frame_id++;

            mxo_canvas_t canvas = { (uint16_t *)fb->buf, (int)fb->width, (int)fb->height };
            draw_results(&canvas, &res);

            char text[24];
            snprintf(text, sizeof(text), "%s  %d FPS", ctx.fn < 3 ? k_fn_names[ctx.fn] : "?", fps);
            mxo_draw_text(&canvas, 2, 2, text, MXO_WHITE, MXO_BLACK);
            if (s_boot_error) mxo_draw_text(&canvas, 2, canvas.height - 10, s_boot_error, MXO_WHITE, MXO_RED);

            /* Connection icon: filled square = host talked to us within 3 s, hollow = not connected. */
            int ix = canvas.width - ICON_SIZE - ICON_MARGIN;
            mxo_fill_rect(&canvas, ix - 2, ICON_MARGIN - 2, ICON_SIZE + 4, ICON_SIZE + 4, MXO_BLACK);
            if (ctx.connected)
                mxo_fill_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, MXO_WHITE);
            else
                mxo_draw_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, 2, MXO_WHITE);

            if (mxcam_display_draw((const uint16_t *)fb->buf) != ESP_OK) lcd_err++;
            mxcam_camera_release(fb);
            status_led_set(res.count > 0 ? STATUS_LED_RESULT : STATUS_LED_IDLE, ctx.cfg.led);
            frames++;
        }

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            fps = (int)((int64_t)frames * 1000000 / (now - t0));
            encoder_stats_t es;
            encoder_take_stats(&es);
            uint32_t q_drop, q_big, tx_fail;
            mxcam_link_take_push_stats(&q_drop, &q_big, &tx_fail);
            unsigned proc_avg_ms = frames ? (unsigned)(proc_sum_us / (uint64_t)frames / 1000u) : 0;
            ESP_LOGI(TAG,
                     "fps=%d fn=%s proc_ms=%u/%u results=%u stream=%d | enc_ms=%u/%u jpeg=%u sent=%u "
                     "drop=%u reenc=%u big=%u enc_fail=%u | qdrop=%u qbig=%u txfail=%u rfail=%u | "
                     "heap=%u min=%u psram=%u stack_hw=%u enc_stack_hw=%u | grab_fail=%u bad_size=%u lcd_err=%u",
                     fps, last_fn < 3 ? k_fn_names[last_fn] : "?", proc_avg_ms, (unsigned)(proc_max_us / 1000u),
                     last_count, streaming_now ? 1 : 0, (unsigned)es.enc_avg_ms, (unsigned)es.enc_max_ms,
                     (unsigned)es.jpeg_avg, es.frames, es.send_fail, es.reenc, es.too_big, es.enc_fail,
                     (unsigned)q_drop, (unsigned)q_big, (unsigned)tx_fail, results_push_fail,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)es.stack_hw, grab_fail, bad_size,
                     lcd_err);
            frames = 0;
            proc_sum_us = 0;
            proc_max_us = 0;
            t0 = now;
        }
    }
}

esp_err_t pipeline_start(mxs_state_t *state, const char *boot_error)
{
    s_state = state;
    s_boot_error = boot_error;
    if (xTaskCreatePinnedToCore(pipeline_task, "pipeline", PIPE_STACK, NULL, PIPE_PRIO, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
