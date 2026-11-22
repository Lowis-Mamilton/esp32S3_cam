/* BLE host port (ADR-0022, docs/protocol.md "BLE"): the same MX packets as
 * USB, carried as a byte stream over one GATT service. NimBLE, peripheral
 * only, one connection, no pairing.
 *
 *   RX characteristic (write / write without response): request bytes.
 *   TX characteristic (notify): responses and pushes, cut into notifications.
 *
 * Lessons from the spike (docs/measurements/ble-spike.md), all three matter:
 *   - Ask for nothing at connect (no PHY, data length or interval request).
 *     Requests right after connecting collided with the central's own
 *     start-up procedures: Windows service discovery took 10-60 s.
 *   - Never ask for the 2M PHY: it broke the link on the test PC.
 *   - Leave mbufs for the host: it needs them to cut notifications into
 *     link-layer packets and to answer the central. */
#include "mxcam_ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mxcam_link.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "mxble";

/* 4d584300-00xx-4b1e-9c5a-7f3e2a1d0c9b: xx = 10 service, 11 RX, 12 TX. */
#define MXBLE_UUID(x) \
    BLE_UUID128_INIT(0x9b, 0x0c, 0x1d, 0x2a, 0x3e, 0x7f, 0x5a, 0x9c, 0x1e, 0x4b, (x), 0x00, 0x00, 0x43, 0x58, 0x4d)
static const ble_uuid128_t s_svc_uuid = MXBLE_UUID(0x10);
static const ble_uuid128_t s_rx_uuid = MXBLE_UUID(0x11);
static const ble_uuid128_t s_tx_uuid = MXBLE_UUID(0x12);

#define RX_STREAM_SIZE 2048 /* a request is at most 1024 + 9 bytes */
#define MAX_WRITE 512       /* largest ATT value */
#define MAX_CHUNK 495       /* + 3 ATT + 4 L2CAP = exactly two 251-byte link-layer packets */
#define MIN_FREE_MBUFS 10   /* never take the host's last mbufs */

/* Measured: ~57 KB/s (1M PHY, 15 ms interval, 251-byte packets). Budgets
 * assume ~40 KB/s plus room for a response queued ahead. */
#define REPLY_BUDGET_US (300 * 1000)
#define PUSH_BUDGET_BASE_US (150 * 1000)
#define PUSH_BUDGET_PER_KB_US (25 * 1000)
#define MUTEX_WAIT_MS 500

static uint8_t s_own_addr_type;
static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static char s_name[16];
static StreamBufferHandle_t s_rx;
static StaticStreamBuffer_t s_rx_ctl;
EXT_RAM_BSS_ATTR static uint8_t s_rx_storage[RX_STREAM_SIZE + 1];

static void advertise(void);

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn, (void)attr, (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[MAX_WRITE];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    /* Never blocks the host task. If the link's RX task is that far behind,
     * the bytes are dropped; the decoder resyncs and the host retries. */
    xStreamBufferSend(s_rx, buf, n, 0);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn, (void)attr, (void)ctxt, (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED; /* notify only */
}

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_svc_uuid.u,
      .characteristics =
          (struct ble_gatt_chr_def[]){
              { .uuid = &s_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
              { .uuid = &s_tx_uuid.u,
                .access_cb = tx_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle },
              { 0 } } },
    { 0 },
};

static void peer_gone(void)
{
    s_subscribed = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    mxcam_link_port_closed();
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise();
            break;
        }
        /* Nothing is requested from the central here (see the file comment). */
        s_conn = ev->connect.conn_handle;
        xStreamBufferReset(s_rx); /* no half request left over from the last host */
        ESP_LOGI(TAG, "connected");
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", ev->disconnect.reason);
        peer_gone();
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) s_subscribed = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    /* Name in the advertisement (Studio filters on the "MXCam-" prefix), the
     * 128-bit service UUID in the scan response. */
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)s_name;
    f.name_len = (uint8_t)strlen(s_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = &s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params ap = { 0 };
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (rc == 0) rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "advertise failed: %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no address: %d", rc);
        return;
    }
    advertise();
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int port_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    return (int)xStreamBufferReceive(s_rx, buf, cap, pdMS_TO_TICKS(timeout_ms));
}

static bool port_write(const uint8_t *data, size_t len, int64_t deadline_us)
{
    size_t off = 0;
    while (off < len) {
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return false;
        int rc = BLE_HS_ENOMEM;
        size_t n = 0;
        if (os_msys_num_free() >= MIN_FREE_MBUFS) {
            size_t room = (size_t)ble_att_mtu(conn) - 3;
            if (room > MAX_CHUNK) room = MAX_CHUNK;
            n = len - off < room ? len - off : room;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
            if (om) rc = ble_gatts_notify_custom(conn, s_tx_handle, om);
        }
        if (rc == 0) {
            off += n;
        } else if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
            /* Host buffers full: the radio has not drained them yet. */
            if (esp_timer_get_time() >= deadline_us) return false;
            vTaskDelay(1);
        } else {
            return false; /* disconnected or not subscribed any more */
        }
    }
    return true;
}

static uint32_t push_budget_us(size_t len)
{
    return PUSH_BUDGET_BASE_US + (uint32_t)(len * PUSH_BUDGET_PER_KB_US / 1024u);
}

static const mxcam_link_port_t s_port = {
    .read = port_read,
    .write = port_write,
    .reply_budget_us = REPLY_BUDGET_US,
    .push_budget_us = push_budget_us,
    .mutex_wait_ms = MUTEX_WAIT_MS,
};

esp_err_t mxcam_ble_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "MXCam-%02X%02X", mac[4], mac[5]);

    s_rx = xStreamBufferCreateStatic(RX_STREAM_SIZE, 1, s_rx_storage, &s_rx_ctl);
    if (!s_rx) return ESP_ERR_NO_MEM;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt setup: %d", rc);
        return ESP_FAIL;
    }
    err = mxcam_link_add_port(&s_port);
    if (err != ESP_OK) return err;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

const char *mxcam_ble_name(void)
{
    return s_name;
}
