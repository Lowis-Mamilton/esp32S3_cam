#include "pipeline.h"

#include <stdio.h>

#include "esp_log.h"
#include "encoder.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_camera.h"
#include "mxcam_display.h"
#include "mxcam_link.h"
#include "mxcam_overlay.h"

static const char *TAG = "pipeline";

#define ICON_SIZE 12
#define ICON_MARGIN 4

static void pipeline_task(void *arg)
{
    mxs_state_t *state = arg;
    int64_t t0 = esp_timer_get_time();
    int frames = 0;
    int fps = 0;
    /* Cumulative error counters, logged together with fps once per second. */
    unsigned grab_fail = 0, bad_size = 0, lcd_err = 0;
    uint32_t frame_id = 0;
    /* Streaming schedule: next_due_ms advances by one interval per submitted
     * frame (not "now + interval") so the average rate matches max_fps even
     * when the frame period (~91 ms) does not divide the interval. */
    uint32_t next_due_ms = 0;
    bool was_wanting = false;
    bool streaming_now = false;

    for (;;) {
        camera_fb_t *fb = mxcam_camera_grab();
        if (!fb) {
            grab_fail++;
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (fb->width != MXCAM_LCD_W || fb->height != MXCAM_LCD_H ||
                   fb->len < (size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2) {
            /* mxcam_display_draw assumes a full 320x240 RGB565 buffer. */
            bad_size++;
            mxcam_camera_release(fb);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool connected, want;
            uint8_t eff_fps, quality;
            mxcam_link_state_lock();
            mxs_tick(state, now_ms);
            connected = mxs_host_connected(state, now_ms);
            want = mxs_wants_frames(state);
            streaming_now = state->streaming;
            eff_fps = mxs_effective_fps(state);
            quality = mxs_effective_quality(state);
            mxcam_link_state_unlock();

            /* JPEG work happens on the encoder task (core 0). Here we only
             * copy the clean frame (before the overlay) when a push is due
             * and the encoder is idle; otherwise the frame is simply skipped. */
            if (want) {
                if (!was_wanting) next_due_ms = now_ms;
                if ((int32_t)(now_ms - next_due_ms) >= 0 &&
                    encoder_try_submit((const uint16_t *)fb->buf, frame_id, now_ms, quality)) {
                    uint32_t interval = 1000u / eff_fps;
                    next_due_ms += interval;
                    if ((int32_t)(now_ms - next_due_ms) > (int32_t)interval) next_due_ms = now_ms;
                }
            }
            was_wanting = want;
            frame_id++;

            mxo_canvas_t canvas = { (uint16_t *)fb->buf, (int)fb->width, (int)fb->height };

            char text[24];
            snprintf(text, sizeof(text), "NONE  %d FPS", fps);
            mxo_draw_text(&canvas, 2, 2, text, MXO_WHITE, MXO_BLACK);

            /* Connection icon: filled square = host talked to us within 3 s, hollow = not connected. */
            int ix = canvas.width - ICON_SIZE - ICON_MARGIN;
            mxo_fill_rect(&canvas, ix - 2, ICON_MARGIN - 2, ICON_SIZE + 4, ICON_SIZE + 4, MXO_BLACK);
            if (connected)
                mxo_fill_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, MXO_WHITE);
            else
                mxo_draw_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, 2, MXO_WHITE);

            if (mxcam_display_draw((const uint16_t *)fb->buf) != ESP_OK) lcd_err++;
            mxcam_camera_release(fb);
            frames++;
        }

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            fps = (int)((int64_t)frames * 1000000 / (now - t0));
            encoder_stats_t es;
            encoder_take_stats(&es);
            ESP_LOGI(TAG,
                     "fps=%d grab_fail=%u bad_size=%u lcd_err=%u stream=%d sent=%u drop=%u enc_fail=%u "
                     "enc_avg_ms=%u enc_max_ms=%u jpeg_avg=%u",
                     fps, grab_fail, bad_size, lcd_err, streaming_now ? 1 : 0, es.frames, es.send_fail,
                     es.enc_fail, (unsigned)es.enc_avg_ms, (unsigned)es.enc_max_ms, (unsigned)es.jpeg_avg);
            frames = 0;
            t0 = now;
        }
    }
}

void pipeline_start(mxs_state_t *state)
{
    xTaskCreatePinnedToCore(pipeline_task, "pipeline", 6144, state, 5, NULL, 1);
}
