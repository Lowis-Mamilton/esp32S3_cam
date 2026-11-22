#include "mxcam_link.h"

#include <stdint.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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

#define MAX_FRAME_PAYLOAD PUSHQ_MAX_FRAME_PAYLOAD /* hard cap: larger FRAMEs are dropped and counted */
#define MUTEX_WAIT_MS 150 /* how long a sender may wait for the tx mutex (a big push holds it) */
#define MAX_PORTS 2 /* 0 = USB (always there), 1 = the optional second host port (BLE) */
#define USB_PORT 0
#define MAX_RESULTS_PAYLOAD 512u
#define FRAME_SLOTS 3   /* one being sent + at most 2 pending */
#define RESULTS_SLOTS 2 /* one being sent + the latest */
#define MAX_SLOTS 3

/* Push queues: FRAME keeps 2 pending (drops the oldest), RESULTS only the
 * latest. Payloads are copied in by the producer; the TX task sends them. */
typedef struct {
    pushq_t q;
    uint8_t *buf[MAX_SLOTS];
    uint16_t len[MAX_SLOTS];
    size_t cap;
    uint8_t cmd;
} push_kind_t;

/* One host port: where its bytes come from and go to, and its own tx mutex
 * (a packet is never interleaved with another one on the same port). */
typedef struct {
    const mxcam_link_port_t *io;
    SemaphoreHandle_t tx_mutex;
} port_t;

static mxs_state_t *s_state;
static SemaphoreHandle_t s_state_mutex;
static port_t s_ports[MAX_PORTS];
/* The port of the latest valid request. Pushes and deferred responses go
 * there: one host at a time (ADR-0022). Written by the RX tasks. */
static volatile int s_active = USB_PORT;
static push_kind_t s_results_kind, s_frame_kind;
static portMUX_TYPE s_push_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_tx_task;
static uint8_t *s_tx_buf;
static uint8_t s_push_seq; /* MXCam's own push counter (protocol.md 3), TX task only */
static SemaphoreHandle_t s_flush_mutex;
static uint32_t s_dropped, s_dropped_size, s_tx_fail;

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

static int usb_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(timeout_ms));
}

static bool usb_write(const uint8_t *data, size_t len, int64_t deadline_us)
{
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
        if (off < len && esp_timer_get_time() >= deadline_us) return false;
    }
    return true;
}

static const mxcam_link_port_t s_usb_io = {
    .read = usb_read,
    .write = usb_write,
    .reply_budget_us = SEND_BUDGET_US,
    .push_budget_us = pushq_send_budget_us,
    .mutex_wait_ms = MUTEX_WAIT_MS,
};

/* Holds the port's tx mutex for the whole packet (a response is never
 * interleaved inside another packet's bytes). The transfer budget starts once
 * the mutex is ours; waiting for it is bounded separately by mutex_wait_ms. */
static bool send_packet(int port, const uint8_t *data, size_t len, uint32_t budget_us)
{
    port_t *p = &s_ports[port];
    if (!p->io || !p->tx_mutex || !data || len == 0) return false;

    if (xSemaphoreTake(p->tx_mutex, pdMS_TO_TICKS(p->io->mutex_wait_ms)) != pdTRUE) return false;
    bool ok = p->io->write(data, len, esp_timer_get_time() + budget_us);
    xSemaphoreGive(p->tx_mutex);
    return ok;
}

bool mxcam_link_send(const uint8_t *data, size_t len)
{
    int port = s_active;
    return send_packet(port, data, len, s_ports[port].io ? s_ports[port].io->reply_budget_us : 0);
}

void mxcam_link_flush_deferred(void)
{
    uint8_t out[MXS_DEFERRED_PKT], seq;
    /* One flusher at a time (RX or TX task), so ack always pops the packet that was sent. */
    if (!s_flush_mutex || xSemaphoreTake(s_flush_mutex, 0) != pdTRUE) return;
    for (int i = 0; i < MXS_DEFERRED_QUEUE; i++) {
        size_t n = 0;
        mxcam_link_state_lock();
        if (s_state) n = mxs_peek_deferred(s_state, &seq, out, sizeof(out));
        mxcam_link_state_unlock();
        if (n == 0) break;
        if (!mxcam_link_send(out, n)) break; /* kept queued: retried at the next flush */
        mxcam_link_state_lock();
        mxs_ack_deferred(s_state);
        mxcam_link_state_unlock();
    }
    xSemaphoreGive(s_flush_mutex);
}

void mxcam_link_kick_deferred(void)
{
    /* The TX task flushes the outbox on every wakeup. Non-blocking. */
    if (s_tx_task) xTaskNotifyGive(s_tx_task);
}

void mxcam_link_drop_results_locked(void)
{
    portENTER_CRITICAL(&s_push_lock);
    pushq_drop_pending(&s_results_kind.q);
    portEXIT_CRITICAL(&s_push_lock);
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
    if (!k || !s_tx_task || (!payload && len > 0)) return false;
    if (len > k->cap) {
        portENTER_CRITICAL(&s_push_lock);
        s_dropped_size++;
        portEXIT_CRITICAL(&s_push_lock);
        return false;
    }

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

void mxcam_link_take_push_stats(uint32_t *dropped, uint32_t *dropped_size, uint32_t *tx_fail)
{
    portENTER_CRITICAL(&s_push_lock);
    *dropped = s_dropped;
    *dropped_size = s_dropped_size;
    *tx_fail = s_tx_fail;
    s_dropped = s_dropped_size = s_tx_fail = 0;
    portEXIT_CRITICAL(&s_push_lock);
}

static void tx_task(void *arg)
{
    (void)arg;
    push_kind_t *order[2] = { &s_results_kind, &s_frame_kind }; /* small, latency-sensitive first */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Deferred responses (LEARN results / cancels) first: small and awaited. */
        mxcam_link_flush_deferred();
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
            /* Whole packet under one mutex hold (nothing interleaves inside it),
             * with a length-scaled budget, to the host that spoke last. */
            int port = s_active;
            bool ok = n > 0 && send_packet(port, s_tx_buf, n, s_ports[port].io->push_budget_us(n));

            portENTER_CRITICAL(&s_push_lock);
            pushq_done(&k->q, idx);
            if (!ok) s_tx_fail++;
            portEXIT_CRITICAL(&s_push_lock);
        }
    }
}

/* One RX task per port: its own decoder and buffers. `arg` is the port index. */
static void rx_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    const mxcam_link_port_t *io = s_ports[port].io;
    static uint8_t usb_payload_buf[MXP_MAX_REQUEST_PAYLOAD];
    static uint8_t usb_out[MXS_RESPONSE_MAX]; /* largest: GET_LEARNED with 32 tag names */
    uint8_t *payload_buf = usb_payload_buf, *out = usb_out;
    if (port != USB_PORT) {
        /* The second port's buffers live in PSRAM: internal RAM is scarce. */
        payload_buf = heap_caps_malloc(MXP_MAX_REQUEST_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        out = heap_caps_malloc(MXS_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!payload_buf || !out) {
            ESP_LOGE("mxlink", "no memory for port %d", port);
            vTaskDelete(NULL);
        }
    }
    uint8_t rx[64];
    mxp_decoder_t dec;
    mxp_decoder_init(&dec, payload_buf, MXP_MAX_REQUEST_PAYLOAD);

    for (;;) {
        int n = io->read(rx, sizeof(rx), 100);
        /* Deferred responses (LEARN results, cancels after a timeout or
         * function change) go out here too, also on idle wakeups (n <= 0). */
        mxcam_link_flush_deferred();
        for (int i = 0; i < n; i++) {
            mxp_frame_t req;
            if (!mxp_decoder_feed(&dec, rx[i], &req)) continue;
            s_active = port; /* this host is the one talking now */
            mxcam_link_state_lock();
            size_t m = mxs_handle(s_state, &req, now_ms(), out, MXS_RESPONSE_MAX);
            /* Function changed: queued RESULTS were computed for the old one. */
            if (mxs_take_results_invalidated(s_state)) mxcam_link_drop_results_locked();
            mxcam_link_state_unlock();
            /* MXS_DEFERRED: the answer is queued in the outbox later (pipeline /
             * store worker) and sent by mxcam_link_flush_deferred. A send
             * failure here: host will time out and retry. */
            if (m > 0 && m != MXS_DEFERRED) send_packet(port, out, m, io->reply_budget_us);
            mxcam_link_flush_deferred();
        }
    }
}

esp_err_t mxcam_link_add_port(const mxcam_link_port_t *io)
{
    port_t *p = &s_ports[1];
    if (!s_state || !io || !io->read || !io->write || !io->push_budget_us) return ESP_ERR_INVALID_ARG;
    if (p->io) return ESP_ERR_INVALID_STATE; /* only one extra port */
    p->tx_mutex = xSemaphoreCreateMutex();
    if (!p->tx_mutex) return ESP_ERR_NO_MEM;
    /* This task's stack is in PSRAM: internal RAM is at its floor in the
     * Bluetooth build, and the task only decodes requests and sends replies.
     * It must never call flash functions itself (cache off = stack gone);
     * SAVE and FACTORY_RESET already run in the store worker. */
    static StaticTask_t tcb;
    StackType_t *stack = heap_caps_malloc(RX_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) return ESP_ERR_NO_MEM;
    p->io = io;
    if (!xTaskCreateStaticPinnedToCore(rx_task, "mxlink_rx2", RX_TASK_STACK, (void *)(intptr_t)1, RX_TASK_PRIO, stack,
                                       &tcb, 0)) {
        p->io = NULL;
        heap_caps_free(stack);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mxcam_link_port_closed(void)
{
    s_active = USB_PORT;
}

esp_err_t mxcam_link_start(mxs_state_t *state)
{
    s_state = state;
    s_state_mutex = xSemaphoreCreateMutex();
    s_ports[USB_PORT].tx_mutex = xSemaphoreCreateMutex();
    s_ports[USB_PORT].io = &s_usb_io;
    s_flush_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex || !s_ports[USB_PORT].tx_mutex || !s_flush_mutex) return ESP_ERR_NO_MEM;

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
    if (xTaskCreatePinnedToCore(rx_task, "mxlink_rx", RX_TASK_STACK, (void *)(intptr_t)USB_PORT, RX_TASK_PRIO, NULL,
                                0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
