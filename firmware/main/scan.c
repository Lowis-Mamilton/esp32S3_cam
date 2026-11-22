#include "scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "mxcam_link.h"
#include "mxcam_motion.h"
#include "mxcam_qr.h"
#include "mxcam_shape.h"
#include "mxcam_store.h"

static const char *TAG = "scan";

/* Studio's ID palette (pc-app/src/theme.css --id-1..--id-8), as pipeline.c's
 * k_id_colors, in the panel's big-endian byte order. */
#define HEX565(rgb) ((uint16_t)(((((rgb) >> 19) & 0x1F) << 11) | ((((rgb) >> 10) & 0x3F) << 5) | (((rgb) >> 3) & 0x1F)))
#define ID_COLOR(rgb) MXO_RGB565(HEX565(rgb))
static const uint16_t k_id_colors[8] = {
    ID_COLOR(0xef4b4b), ID_COLOR(0xa3d62f), ID_COLOR(0x34d27b), ID_COLOR(0x2cc8d8),
    ID_COLOR(0x4d8ef5), ID_COLOR(0x8f6df2), ID_COLOR(0xd45be0), ID_COLOR(0xf2609b),
};
#define BOX_THICKNESS 2
#define QR_LINE_THICKNESS 2

/* Copies of the shared data (scan_sync, under the state lock). Only n_qr and
 * qr[] of s_qr_data are used (mxst_qr_lookup reads nothing else); PSRAM like
 * the other per-function copies (~4 KB, internal RAM is the scarce one). */
static bool s_synced;
static uint32_t s_gen;
static mxp_shape_config_t s_shape_cfg;
static mxp_motion_config_t s_motion_cfg;
static EXT_RAM_BSS_ATTR mxs_data_t s_qr_data;

/* The last scan_run's decoded codes (function 6), in record order: their
 * content for GET_RESULT_DATA, LEARN 6 and the TFT labels. */
static EXT_RAM_BSS_ATTR mxq_code_t s_codes[MXQ_MAX_CODES];
static uint8_t s_n_codes;
static EXT_RAM_BSS_ATTR mxs_rdata_t s_rdata[MXQ_MAX_CODES];

/* Per-second QR stats (scan_log_stats). */
static uint64_t s_qr_sum_us, s_qr_gray_us, s_qr_id_us, s_qr_dec_us;
static uint32_t s_qr_max_us;
static unsigned s_qr_runs, s_qr_skip, s_qr_grids, s_qr_ok, s_qr_fail;

void scan_sync(const mxs_state_t *s)
{
    if (s_synced && s->data_gen == s_gen) return;
    s_synced = true;
    s_gen = s->data_gen;
    const mxs_data_t *d = s->data;
    s_shape_cfg = d->shape_cfg;
    s_motion_cfg = d->motion_cfg;
    s_qr_data.n_qr = d->n_qr;
    memcpy(s_qr_data.qr, d->qr, sizeof(s_qr_data.qr));
}

void scan_on_function(uint8_t fn)
{
    if (fn == MXP_FUNC_MOTION) mxm_reset();
    s_n_codes = 0;
}

static int run_qr(const uint16_t *frame, mxp_result_t *out, uint8_t cap)
{
    mxq_stats_t st;
    int n = mxq_detect(frame, s_codes, &st);
    if (n == MXQ_SKIPPED) {
        s_n_codes = 0;
        s_qr_skip++;
        return -1;
    }
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        const mxq_code_t *c = &s_codes[i];
        mxq_to_result(c, mxst_qr_lookup(&s_qr_data, c->bytes, c->len, c->total_len), &out[i]);
    }
    s_n_codes = (uint8_t)n;
    s_qr_sum_us += st.total_us;
    s_qr_gray_us += st.gray_us;
    s_qr_id_us += st.identify_us;
    s_qr_dec_us += st.decode_us;
    if (st.total_us > s_qr_max_us) s_qr_max_us = st.total_us;
    s_qr_runs++;
    s_qr_grids += st.grids;
    s_qr_ok += st.decoded;
    s_qr_fail += st.failed;
    return n;
}

int scan_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out, uint8_t cap)
{
    if (fn != MXP_FUNC_QR) s_n_codes = 0;
    switch (fn) {
    case MXP_FUNC_QR: return run_qr(frame, out, cap);
    case MXP_FUNC_SHAPE: return mxsh_detect(frame, &s_shape_cfg, out, cap);
    case MXP_FUNC_MOTION: return mxm_detect(frame, &s_motion_cfg, out, cap, NULL);
    default: return 0;
    }
}

void scan_publish_data(mxs_state_t *s, uint8_t fn)
{
    if (fn != MXP_FUNC_QR || s_n_codes == 0) return;
    for (int i = 0; i < s_n_codes; i++) {
        const mxq_code_t *c = &s_codes[i];
        mxs_rdata_t *r = &s_rdata[i];
        r->flags = c->total_len > c->len ? MXP_DATA_FLAG_TRUNCATED : 0;
        r->total_len = c->total_len;
        r->len = c->len;
        memcpy(r->data, c->bytes, c->len);
    }
    mxs_set_result_data(s, s_rdata, s_n_codes);
}

bool scan_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req)
{
    int i = mxq_pick_for_learn(s_codes, s_n_codes, req->x, req->y, req->w, req->h);
    const mxq_code_t *c = i >= 0 ? &s_codes[i] : NULL;
    mxcam_link_state_lock();
    bool queued = mxs_finish_learn_qr(s, seq, c ? c->bytes : NULL, c ? c->len : 0, c ? c->total_len : 0);
    scan_sync(s); /* the new id applies from the next frame */
    uint16_t id = c ? mxst_qr_lookup(s->data, c->bytes, c->len, c->total_len) : 0;
    mxcam_link_state_unlock();
    if (c) {
        char label[24];
        mxq_label(id, c, label, sizeof(label));
        ESP_LOGI(TAG, "learn qr seq=%u -> id %u (%u bytes, \"%s\")", seq, id, c->total_len, label);
    } else {
        ESP_LOGI(TAG, "learn qr seq=%u: no decoded code in %u,%u %ux%u", seq, req->x, req->y, req->w, req->h);
    }
    return queued;
}

/* Label above the box's left edge as seen on the panel (inside near the top
 * edge when there is no room above), drawn mirrored (ADR-0010), as
 * pipeline.c's draw_results. */
static void draw_label(mxo_canvas_t *c, int x0, int y0, int w, const char *label, uint16_t col)
{
    int tw = (int)strlen(label) * 8;
    int lx = mxo_mirror_x(c, x0, w);
    if (lx + tw > c->width) lx = c->width - tw;
    if (lx < 0) lx = 0;
    int ly = y0 - 9;
    if (ly < 0) ly = y0 + BOX_THICKNESS + 1;
    if (ly < 0) ly = 0;
    if (ly + 8 > c->height) ly = c->height - 8;
    mxo_draw_text_mirrored(c, lx, ly, label, MXO_BLACK, col);
}

/* Left / top edge of a record's box (centre = (min + max) / 2 rounded down,
 * w = max - min + 1, so min = centre - (w - 1) / 2). */
static int box_x0(const mxp_result_t *r) { return r->x - (r->w > 0 ? (r->w - 1) / 2 : 0); }
static int box_y0(const mxp_result_t *r) { return r->y - (r->h > 0 ? (r->h - 1) / 2 : 0); }

static void draw_qr(mxo_canvas_t *c, const mxp_results_t *res)
{
    /* res->r[i] was made from s_codes[i] (same frame) unless the batch was
     * dropped (count 0) meanwhile. */
    int n = res->count < s_n_codes ? res->count : s_n_codes;
    for (int i = 0; i < n; i++) {
        const mxp_result_t *r = &res->r[i];
        uint16_t col = r->id >= 1 ? k_id_colors[(r->id - 1) % 8] : MXO_WHITE;
        mxp_corners_extra_t e;
        if (mxp_unpack_corners_extra(r, &e))
            for (int k = 0; k < 4; k++)
                mxo_draw_line(c, e.x[k], e.y[k], e.x[(k + 1) % 4], e.y[(k + 1) % 4], QR_LINE_THICKNESS, col);
        char label[24];
        mxq_label(r->id, &s_codes[i], label, sizeof(label));
        draw_label(c, box_x0(r), box_y0(r), r->w, label, col);
    }
}

static void draw_boxes(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res)
{
    for (int i = 0; i < res->count; i++) {
        const mxp_result_t *r = &res->r[i];
        uint16_t col;
        char label[16];
        if (fn == MXP_FUNC_SHAPE) {
            col = r->id >= 1 ? k_id_colors[(r->id - 1) % 8] : MXO_WHITE;
            mxsh_label(r->id, r->confidence, label, sizeof(label));
        } else {
            col = MXO_WHITE;
            snprintf(label, sizeof(label), "MOVE %u", (unsigned)r->confidence);
        }
        int x0 = box_x0(r), y0 = box_y0(r);
        mxo_draw_rect(c, x0, y0, r->w, r->h, BOX_THICKNESS, col); /* clips to the canvas */
        draw_label(c, x0, y0, r->w, label, col);
    }
}

void scan_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res)
{
    if (fn == MXP_FUNC_QR)
        draw_qr(c, res);
    else if (fn == MXP_FUNC_SHAPE || fn == MXP_FUNC_MOTION)
        draw_boxes(c, fn, res);
}

/* "12.3" from microseconds. */
static void fmt_ms(char *buf, size_t cap, uint64_t us)
{
    snprintf(buf, cap, "%u.%u", (unsigned)(us / 1000u), (unsigned)(us / 100u % 10u));
}

void scan_log_stats(const char *tag)
{
    if (!s_qr_runs && !s_qr_skip) return;
    unsigned r = s_qr_runs ? s_qr_runs : 1;
    char g[12], id[12], dec[12];
    fmt_ms(g, sizeof(g), s_qr_gray_us / r);
    fmt_ms(id, sizeof(id), s_qr_id_us / r);
    fmt_ms(dec, sizeof(dec), s_qr_dec_us / r);
    /* qr_ms: total avg/max/runs; gray, identify (quirc_end) and decode
     * averages; grids found, decoded and failed summed over the second. */
    ESP_LOGI(tag, "qr_ms=%u/%u/%u gray=%s id=%s dec=%s grids=%u ok=%u fail=%u skip=%u",
             (unsigned)(s_qr_sum_us / r / 1000u), (unsigned)(s_qr_max_us / 1000u), s_qr_runs, g, id, dec,
             s_qr_grids, s_qr_ok, s_qr_fail, s_qr_skip);
    s_qr_sum_us = s_qr_gray_us = s_qr_id_us = s_qr_dec_us = 0;
    s_qr_max_us = 0;
    s_qr_runs = s_qr_skip = s_qr_grids = s_qr_ok = s_qr_fail = 0;
}
