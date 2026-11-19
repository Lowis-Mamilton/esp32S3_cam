#include "store_worker.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_link.h"
#include "mxcam_store.h"

static const char *TAG = "store";

#define WORKER_STACK 6144 /* mxst_save: NVS internals ~3 KB; snapshot is static */
#define WORKER_PRIO 2     /* below the encoder (3), link TX (4) and RX (6) */
#define POLL_MS 50

static mxs_state_t *s_state;
static bool s_tick;

static void worker_task(void *arg)
{
    (void)arg;
    static mxs_data_t snap; /* ~1.9 KB: keep it off the stack */
    uint8_t out[16], cmd = 0, seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        mxcam_link_state_lock();
        if (s_tick) mxs_tick(s_state, (uint32_t)(esp_timer_get_time() / 1000));
        bool got = mxs_take_pending_store(s_state, &cmd, &seq, &snap);
        mxcam_link_state_unlock();
        if (s_tick) mxcam_link_flush_cancelled();
        if (!got) continue;

        int64_t t0 = esp_timer_get_time();
        esp_err_t e = cmd == MXP_CMD_SAVE ? mxst_save(&snap) : mxst_erase();
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "%s done in %u ms", cmd == MXP_CMD_SAVE ? "save" : "factory reset", (unsigned)ms);
        else
            ESP_LOGE(TAG, "%s failed: %s", cmd == MXP_CMD_SAVE ? "save" : "factory reset", esp_err_to_name(e));

        mxcam_link_state_lock();
        size_t n = mxs_complete_store(s_state, seq, e == ESP_OK, out, sizeof(out));
        mxcam_link_state_unlock();
        /* Every claimed store op completes; a send failure means the host times out. */
        if (n > 0) mxcam_link_send(out, n);
    }
}

esp_err_t store_worker_start(mxs_state_t *state, bool tick)
{
    s_state = state;
    s_tick = tick;
    if (xTaskCreatePinnedToCore(worker_task, "store", WORKER_STACK, NULL, WORKER_PRIO, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
