#include "mxcam_link.h"

#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mxcam_proto.h"

#define TX_RING_SIZE 8192
#define RX_RING_SIZE 2048
#define TX_CHUNK 1024              /* must be <= TX_RING_SIZE: ring sends are all-or-nothing */
#define SEND_BUDGET_US (50 * 1000) /* total time one packet may take, incl. waiting for tx mutex */
#define RX_TASK_STACK 4096
#define RX_TASK_PRIO 6 /* above the encoder (3) so responses are not starved; core 0 only */

static mxs_state_t *s_state;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_tx_mutex;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void mxcam_link_state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

void mxcam_link_state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

bool mxcam_link_send(const uint8_t *data, size_t len)
{
    if (!s_tx_mutex || !data || len == 0) return false;
    int64_t deadline = esp_timer_get_time() + SEND_BUDGET_US;

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(SEND_BUDGET_US / 1000)) != pdTRUE) return false;
    bool ok = true;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > TX_CHUNK) chunk = TX_CHUNK;
        /* One tick (10 ms at 100 Hz) per attempt; the ring drains at USB speed
         * when the host reads, and stays full when it does not. */
        int n = usb_serial_jtag_write_bytes(data + off, chunk, 1);
        if (n > 0) off += (size_t)n;
        /* Checked every iteration so a slowly draining host cannot hold the
         * tx mutex past the budget. */
        if (off < len && esp_timer_get_time() >= deadline) {
            ok = false;
            break;
        }
    }
    xSemaphoreGive(s_tx_mutex);
    return ok;
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t payload_buf[MXP_MAX_REQUEST_PAYLOAD];
    static uint8_t out[128];
    uint8_t rx[64];
    mxp_decoder_t dec;
    mxp_decoder_init(&dec, payload_buf, sizeof(payload_buf));

    for (;;) {
        int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            mxp_frame_t req;
            if (!mxp_decoder_feed(&dec, rx[i], &req)) continue;
            mxcam_link_state_lock();
            size_t m = mxs_handle(s_state, &req, now_ms(), out, sizeof(out));
            mxcam_link_state_unlock();
            if (m > 0) mxcam_link_send(out, m); /* failure: host will time out and retry */
        }
    }
}

esp_err_t mxcam_link_start(mxs_state_t *state)
{
    s_state = state;
    s_state_mutex = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex || !s_tx_mutex) return ESP_ERR_NO_MEM;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_RING_SIZE,
        .rx_buffer_size = RX_RING_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(rx_task, "mxlink_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
