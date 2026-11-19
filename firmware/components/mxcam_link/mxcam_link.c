#include "mxcam_link.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mxcam_proto.h"
#include "pushq.h"

#define TX_RING_SIZE 8192
#define RX_RING_SIZE 2048
#define TX_CHUNK 1024              /* must be <= TX_RING_SIZE: ring sends are all-or-nothing */
#define SEND_BUDGET_US (50 * 1000) /* total time one packet may take, incl. waiting for tx mutex */
#define RX_TASK_STACK 4096
#define RX_TASK_PRIO 6 /* above the encoder (3) so responses are not starved; core 0 only */
#define TX_TASK_STACK 4096
#define TX_TASK_PRIO 4 /* between the encoder (3) and RX (6); core 0 */

#define MAX_FRAME_PAYLOAD 65535u /* protocol limit for one packet's payload */
#define MAX_RESULTS_PAYLOAD 512u
#define FRAME_SLOTS 3   /* one being sent + at most 2 pending */
#define RESULTS_SLOTS 2 /* one being sent + the latest */
#define MAX_SLOTS 3

/* Push queues: FRAME keeps 2 pending (drops the oldest), RESULTS only the
 * latest. Payloads are copied in by the producer; the TX task owns the USB. */
typedef struct {
    pushq_t q;
    uint8_t *buf[MAX_SLOTS];
    uint16_t len[MAX_SLOTS];
    size_t cap;
    uint8_t cmd;
} push_kind_t;

static mxs_state_t *s_state;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_tx_mutex;
static push_kind_t s_results_kind, s_frame_kind;
static portMUX_TYPE s_push_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_tx_task;
static uint8_t *s_tx_buf;
static uint8_t s_push_seq; /* MXCam's own push counter (protocol.md 3), TX task only */
static uint32_t s_dropped, s_tx_fail;

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

void mxcam_link_respond(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[128];
    if (len > sizeof(buf) - MXP_OVERHEAD) return;
    size_t n = mxp_encode(cmd, seq, payload, len, buf, sizeof(buf));
    if (n > 0) mxcam_link_send(buf, n);
}

void mxcam_link_flush_cancelled(void)
{
    uint8_t out[32], seq;
    size_t n = 0;
    mxcam_link_state_lock();
    if (s_state) n = mxs_take_cancelled(s_state, &seq, out, sizeof(out));
    mxcam_link_state_unlock();
    if (n > 0) mxcam_link_send(out, n);
}

static push_kind_t *kind_for(uint8_t cmd)
{
    if (cmd == MXP_CMD_PUSH_FRAME) return &s_frame_kind;
    if (cmd == MXP_CMD_PUSH_RESULTS) return &s_results_kind;
    return NULL;
}

bool mxcam_link_push(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    push_kind_t *k = kind_for(cmd);
    if (!k || !s_tx_task || (!payload && len > 0) || len > k->cap) return false;

    bool dropped;
    portENTER_CRITICAL(&s_push_lock);
    int idx = pushq_acquire(&k->q, &dropped);
    portEXIT_CRITICAL(&s_push_lock);
    if (idx < 0) return false;

    if (len) memcpy(k->buf[idx], payload, len);
    k->len[idx] = len;

    portENTER_CRITICAL(&s_push_lock);
    pushq_commit(&k->q, idx);
    if (dropped) s_dropped++;
    portEXIT_CRITICAL(&s_push_lock);
    xTaskNotifyGive(s_tx_task);
    return true;
}

void mxcam_link_take_push_stats(uint32_t *dropped, uint32_t *tx_fail)
{
    portENTER_CRITICAL(&s_push_lock);
    *dropped = s_dropped;
    *tx_fail = s_tx_fail;
    s_dropped = s_tx_fail = 0;
    portEXIT_CRITICAL(&s_push_lock);
}

static void tx_task(void *arg)
{
    (void)arg;
    push_kind_t *order[2] = { &s_results_kind, &s_frame_kind }; /* small, latency-sensitive first */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            push_kind_t *k = NULL;
            int idx = -1;
            portENTER_CRITICAL(&s_push_lock);
            for (int i = 0; i < 2 && idx < 0; i++) {
                idx = pushq_take(&order[i]->q);
                if (idx >= 0) k = order[i];
            }
            portEXIT_CRITICAL(&s_push_lock);
            if (idx < 0) break;

            /* Each packet takes and releases the tx mutex on its own, so a
             * response from the RX task slips in between packets. */
            size_t n = mxp_encode(k->cmd, s_push_seq++, k->buf[idx], k->len[idx], s_tx_buf,
                                  MAX_FRAME_PAYLOAD + MXP_OVERHEAD);
            bool ok = n > 0 && mxcam_link_send(s_tx_buf, n);

            portENTER_CRITICAL(&s_push_lock);
            pushq_done(&k->q, idx);
            if (!ok) s_tx_fail++;
            portEXIT_CRITICAL(&s_push_lock);
        }
    }
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
        /* Cancelled deferred LEARNs (timeout / function change) go out here,
         * also on idle wakeups (n <= 0). */
        mxcam_link_flush_cancelled();
        for (int i = 0; i < n; i++) {
            mxp_frame_t req;
            if (!mxp_decoder_feed(&dec, rx[i], &req)) continue;
            mxcam_link_state_lock();
            size_t m = mxs_handle(s_state, &req, now_ms(), out, sizeof(out));
            mxcam_link_state_unlock();
            /* MXS_DEFERRED: the pipeline answers later (mxcam_link_send/respond).
             * A send failure: host will time out and retry. */
            if (m > 0 && m != MXS_DEFERRED) mxcam_link_send(out, m);
            mxcam_link_flush_cancelled();
        }
    }
}

esp_err_t mxcam_link_start(mxs_state_t *state)
{
    s_state = state;
    s_state_mutex = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex || !s_tx_mutex) return ESP_ERR_NO_MEM;

    pushq_init(&s_frame_kind.q, FRAME_SLOTS, 2);
    s_frame_kind.cap = MAX_FRAME_PAYLOAD;
    s_frame_kind.cmd = MXP_CMD_PUSH_FRAME;
    pushq_init(&s_results_kind.q, RESULTS_SLOTS, 1);
    s_results_kind.cap = MAX_RESULTS_PAYLOAD;
    s_results_kind.cmd = MXP_CMD_PUSH_RESULTS;
    for (int i = 0; i < FRAME_SLOTS; i++) {
        s_frame_kind.buf[i] = heap_caps_malloc(MAX_FRAME_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame_kind.buf[i]) return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < RESULTS_SLOTS; i++) {
        s_results_kind.buf[i] = heap_caps_malloc(MAX_RESULTS_PAYLOAD, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_results_kind.buf[i]) return ESP_ERR_NO_MEM;
    }
    s_tx_buf = heap_caps_malloc(MAX_FRAME_PAYLOAD + MXP_OVERHEAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_buf) return ESP_ERR_NO_MEM;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_RING_SIZE,
        .rx_buffer_size = RX_RING_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(tx_task, "mxlink_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, &s_tx_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(rx_task, "mxlink_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
