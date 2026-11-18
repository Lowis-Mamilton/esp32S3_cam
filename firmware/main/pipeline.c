#include "pipeline.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_camera.h"
#include "mxcam_display.h"
#include "mxcam_overlay.h"

static const char *TAG = "pipeline";

#define ICON_SIZE 12
#define ICON_MARGIN 4

static void pipeline_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    int frames = 0;
    int fps = 0;

    for (;;) {
        camera_fb_t *fb = mxcam_camera_grab();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        mxo_canvas_t canvas = { (uint16_t *)fb->buf, (int)fb->width, (int)fb->height };

        char text[24];
        snprintf(text, sizeof(text), "NONE  %d FPS", fps);
        mxo_draw_text(&canvas, 2, 2, text, MXO_WHITE, MXO_BLACK);

        /* Connection icon, fixed to "not connected" (hollow square) until Task 5. */
        int ix = canvas.width - ICON_SIZE - ICON_MARGIN;
        mxo_fill_rect(&canvas, ix - 2, ICON_MARGIN - 2, ICON_SIZE + 4, ICON_SIZE + 4, MXO_BLACK);
        mxo_draw_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, 2, MXO_WHITE);

        mxcam_display_draw((const uint16_t *)fb->buf);
        mxcam_camera_release(fb);

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            fps = (int)((int64_t)frames * 1000000 / (now - t0));
            ESP_LOGI(TAG, "fps=%d", fps);
            frames = 0;
            t0 = now;
        }
    }
}

void pipeline_start(void)
{
    xTaskCreatePinnedToCore(pipeline_task, "pipeline", 4096, NULL, 5, NULL, 1);
}
