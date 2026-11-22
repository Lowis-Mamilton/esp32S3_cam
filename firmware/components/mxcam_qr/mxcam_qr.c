#include "mxcam_qr.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "mxcam_gray.h"
#include "quirc.h"

#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_qr needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffers live in PSRAM BSS)"
#endif

static const char *TAG = "mxcam_qr";

static struct quirc *s_q;

/* Per-frame work buffers in PSRAM BSS, not on the stack (ADR-0020 decision 3):
 *   s_code   struct quirc_code   ~3.9 KB (cell bitmap of the largest version)
 *   s_data   struct quirc_data   ~8.9 KB (payload up to QUIRC_MAX_PAYLOAD)
 *   s_found  decoded codes before mxq_sort_limit, ~1.2 KB */
static EXT_RAM_BSS_ATTR struct quirc_code s_code;
static EXT_RAM_BSS_ATTR struct quirc_data s_data;
static EXT_RAM_BSS_ATTR mxq_code_t s_found[MXQ_MAX_GRIDS];

static int16_t to_i16(int v) { return (int16_t)(v < INT16_MIN ? INT16_MIN : (v > INT16_MAX ? INT16_MAX : v)); }

static uint32_t elapsed_us(int64_t from, int64_t to) { return (uint32_t)(to - from); }

bool mxq_init(void)
{
    if (s_q) return true;
    size_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t p0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#if CONFIG_SPIRAM_USE_MALLOC
    /* As mxf_init / mxh_init (ADR-0011): quirc allocates with plain malloc;
     * the grey image alone (76.8 KB) must not land in internal RAM. The
     * switch is global, so call mxq_init before the other tasks start. */
    heap_caps_malloc_extmem_enable(0);
#endif
    struct quirc *q = quirc_new();
    bool ok = q && quirc_resize(q, MXQ_FRAME_W, MXQ_FRAME_H) == 0;
#if CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    if (!ok) {
        if (q) quirc_destroy(q);
        ESP_LOGE(TAG, "quirc allocation failed (internal free %u, PSRAM free %u)", (unsigned)i0, (unsigned)p0);
        return false;
    }
    s_q = q;
    ESP_LOGI(TAG, "qr ready (internal %u -> %u, PSRAM %u -> %u)", (unsigned)i0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)p0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

int mxq_detect(const uint16_t *frame, mxq_code_t out[MXQ_MAX_CODES], mxq_stats_t *stats)
{
    mxq_stats_t st;
    memset(&st, 0, sizeof(st));
    if (!s_q || !frame || !out) {
        if (stats) *stats = st;
        return MXQ_SKIPPED;
    }

    int64_t t0 = esp_timer_get_time();
    int w = 0, h = 0;
    uint8_t *img = quirc_begin(s_q, &w, &h);
    const int npx = w * h; /* MXQ_FRAME_W * MXQ_FRAME_H after mxq_init */
    for (int i = 0; i < npx; i++) img[i] = mxg_luma_be(frame[i]);
    int64_t t1 = esp_timer_get_time();
    quirc_end(s_q);
    int64_t t2 = esp_timer_get_time();

    int count = quirc_count(s_q);
    st.grids = (uint8_t)(count > 255 ? 255 : (count < 0 ? 0 : count));
    uint8_t n = 0;
    for (int i = 0; i < count && i < MXQ_MAX_GRIDS; i++) {
        quirc_extract(s_q, i, &s_code);
        /* No quirc_flip retry: the camera gives the true image (ADR-0010). */
        if (quirc_decode(&s_code, &s_data) != QUIRC_SUCCESS) {
            st.failed++;
            continue;
        }
        mxq_code_t *c = &s_found[n++];
        for (int k = 0; k < 4; k++) {
            c->x[k] = to_i16(s_code.corners[k].x);
            c->y[k] = to_i16(s_code.corners[k].y);
        }
        int len = s_data.payload_len;
        if (len < 0) len = 0;
        if (len > QUIRC_MAX_PAYLOAD) len = QUIRC_MAX_PAYLOAD;
        mxq_set_payload(c, s_data.payload, (size_t)len);
    }
    st.decoded = n;
    n = mxq_sort_limit(s_found, n, MXQ_MAX_CODES);
    memcpy(out, s_found, (size_t)n * sizeof(mxq_code_t));
    int64_t t3 = esp_timer_get_time();

    st.gray_us = elapsed_us(t0, t1);
    st.identify_us = elapsed_us(t1, t2);
    st.decode_us = elapsed_us(t2, t3);
    st.total_us = elapsed_us(t0, t3);
    if (stats) *stats = st;
    return n;
}
