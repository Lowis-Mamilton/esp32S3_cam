#include "tracking.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "mxcam_color.h"
#include "mxcam_line.h"
#include "mxcam_link.h"
#include "mxcam_track.h"

static const char *TAG = "tracking";

/* Studio ID palette entries 4 (cyan, line) and 5 (blue, object), as in
 * pipeline.c's k_id_colors (pc-app theme.css --id-4 / --id-5). */
#define HEX565(rgb) ((uint16_t)(((((rgb) >> 19) & 0x1F) << 11) | ((((rgb) >> 10) & 0x3F) << 5) | (((rgb) >> 3) & 0x1F)))
#define LINE_COLOR MXO_RGB565(HEX565(0x2cc8d8))
#define TRACK_COLOR MXO_RGB565(HEX565(0x4d8ef5))
#define BOX_THICKNESS 2
#define LINE_THICKNESS 3
#define DOT_SIZE 4
#define STATUS_ROW_H 12 /* labels stay below the top-left status text */

/* Copies of the shared data (tracking_sync, under the state lock). */
static bool s_synced;
static uint32_t s_gen;
static mxp_line_config_t s_line_cfg;
static mxp_track_config_t s_track_cfg;
static bool s_line_has_color;
static uint8_t s_line_rgb[3];
static bool s_has_object;
/* ~1.1 KB each: PSRAM like the trackers' own work buffers (internal RAM is
 * the scarce one). s_new receives mxk_learn's output. */
static EXT_RAM_BSS_ATTR mxk_model_t s_model;
static EXT_RAM_BSS_ATTR mxk_model_t s_new;
static EXT_RAM_BSS_ATTR mxk_state_t s_trk;
/* Detector state and the last frame's outcome (for the overlay / status). */
static mxl_state_t s_line;
static mxl_trace_t s_trace;
static bool s_found;

static void rearm(void)
{
    if (s_has_object) mxk_reset(&s_trk, &s_model);
    s_found = true; /* "TRACK" until a frame actually loses it */
}

void tracking_sync(const mxs_state_t *s)
{
    if (s_synced && s->data_gen == s_gen) return;
    s_synced = true;
    s_gen = s->data_gen;
    const mxs_data_t *d = s->data;
    s_line_cfg = d->line_cfg;
    s_track_cfg = d->track_cfg;
    s_line_has_color = d->line_has_color != 0;
    memcpy(s_line_rgb, d->line_rgb, sizeof(s_line_rgb));
    bool has = d->has_object != 0;
    if (has != s_has_object || (has && memcmp(&s_model, &d->object, sizeof(s_model)) != 0)) {
        s_has_object = has;
        if (has) s_model = d->object;
        rearm();
    }
}

void tracking_on_function(uint8_t fn)
{
    (void)fn;
    memset(&s_line, 0, sizeof(s_line));
    memset(&s_trace, 0, sizeof(s_trace));
    rearm();
}

uint8_t tracking_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out)
{
    if (fn == MXP_FUNC_LINE)
        return mxl_detect(frame, &s_line_cfg, s_line_has_color ? s_line_rgb : NULL, &s_line, out, &s_trace);
    if (fn == MXP_FUNC_TRACK) {
        uint8_t n = s_has_object ? mxk_track(frame, &s_model, &s_track_cfg, &s_trk, out) : 0;
        s_found = n > 0;
        return n;
    }
    return 0;
}

bool tracking_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req, const uint16_t *frame)
{
    bool queued;
    if (req->function_id == MXP_FUNC_TRACK) {
        bool ok = mxk_learn(frame, req->x, req->y, req->w, req->h, &s_new);
        mxcam_link_state_lock();
        queued = mxs_finish_learn_object(s, seq, ok ? &s_new : NULL);
        tracking_sync(s); /* track the new object from this very frame */
        mxcam_link_state_unlock();
        if (queued && ok) rearm(); /* also when the same model was learned again */
        if (ok)
            ESP_LOGI(TAG, "learn object seq=%u box=%ux%u tmpl=%ux%u rgb=%u,%u,%u", seq, s_new.box_w, s_new.box_h,
                     s_new.tw, s_new.th, s_new.rgb[0], s_new.rgb[1], s_new.rgb[2]);
        else
            ESP_LOGI(TAG, "learn object seq=%u: too small or no texture", seq);
    } else {
        uint8_t rgb[3];
        bool ok = mxc_sample(frame, req->x, req->y, req->w, req->h, rgb);
        mxcam_link_state_lock();
        queued = mxs_finish_learn(s, seq, ok ? rgb : NULL);
        tracking_sync(s); /* the learned colour (mode 2) applies to this frame */
        mxcam_link_state_unlock();
        if (ok)
            ESP_LOGI(TAG, "learn line seq=%u rgb=%u,%u,%u -> colour %d mode %u", seq, rgb[0], rgb[1], rgb[2],
                     s_line_has_color ? 1 : 0, s_line_cfg.mode);
        else
            ESP_LOGI(TAG, "learn line seq=%u: no colour", seq);
    }
    return queued;
}

/* Label centred on panel x of frame point (fx, fy), above it (below when
 * there is no room), drawn mirrored (ADR-0010). */
static void draw_label_at(mxo_canvas_t *c, int fx, int fy, const char *label, uint16_t col)
{
    int tw = (int)strlen(label) * 8;
    int lx = (c->width - 1 - fx) - tw / 2; /* panel x of the point, centred */
    if (lx + tw > c->width) lx = c->width - tw;
    if (lx < 0) lx = 0;
    int ly = fy - 12;
    if (ly < STATUS_ROW_H) ly = fy + 6;
    if (ly + 8 > c->height) ly = c->height - 8;
    mxo_draw_text_mirrored(c, lx, ly, label, MXO_BLACK, col);
}

/* First ROI row in 320x240 units, as mxl_detect computes it (bottom
 * 120 * roi_pct / 100 rows of the 160x120 image; roi_pct clamped 20-100). */
static int roi_top(const mxp_line_config_t *cfg)
{
    int pct = cfg->roi_pct < 20 ? 20 : cfg->roi_pct > 100 ? 100 : cfg->roi_pct;
    return (120 - 120 * pct / 100) * 2;
}

static int round_cdeg(int cdeg) { return cdeg >= 0 ? (cdeg + 50) / 100 : -((-cdeg + 50) / 100); }

static void draw_line_result(mxo_canvas_t *c, const mxp_results_t *res)
{
    int top = roi_top(&s_line_cfg);
    if (top > 0) mxo_fill_rect(c, 0, top, c->width, 1, MXO_WHITE);
    mxp_line_extra_t e;
    if (res->count < 1 || !mxp_unpack_line_extra(&res->r[0], &e)) return;
    for (int i = 0; i < s_trace.n && i < MXL_MAX_BANDS; i++)
        mxo_fill_rect(c, s_trace.pts[i][0] - DOT_SIZE / 2, s_trace.pts[i][1] - DOT_SIZE / 2, DOT_SIZE, DOT_SIZE,
                      LINE_COLOR);
    mxo_draw_line(c, e.x0, e.y0, e.x1, e.y1, LINE_THICKNESS, LINE_COLOR);
    char label[24];
    snprintf(label, sizeof(label), "A%+d O%+d%s%s", round_cdeg(e.angle_cdeg), e.offset_x,
             (e.flags & MXP_LINE_FLAG_CROSS) ? " X" : "", (e.flags & MXP_LINE_FLAG_BRANCH) ? " Y" : "");
    draw_label_at(c, e.x1, e.y1, label, LINE_COLOR);
}

static void draw_track_result(mxo_canvas_t *c, const mxp_results_t *res)
{
    if (res->count < 1) return;
    const mxp_result_t *r = &res->r[0];
    int x0 = r->x - r->w / 2, y0 = r->y - r->h / 2;
    mxo_draw_rect(c, x0, y0, r->w, r->h, BOX_THICKNESS, TRACK_COLOR); /* clips to the canvas */
    char label[12];
    snprintf(label, sizeof(label), "OBJ %u", (unsigned)r->confidence);
    /* Above the box's left edge as seen on the panel, like the other boxes. */
    int tw = (int)strlen(label) * 8;
    int lx = mxo_mirror_x(c, x0, r->w);
    if (lx + tw > c->width) lx = c->width - tw;
    if (lx < 0) lx = 0;
    int ly = y0 - 9;
    if (ly < 0) ly = y0 + BOX_THICKNESS + 1;
    if (ly < 0) ly = 0;
    if (ly + 8 > c->height) ly = c->height - 8;
    mxo_draw_text_mirrored(c, lx, ly, label, MXO_BLACK, TRACK_COLOR);
}

void tracking_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res)
{
    if (fn == MXP_FUNC_LINE)
        draw_line_result(c, res);
    else if (fn == MXP_FUNC_TRACK)
        draw_track_result(c, res);
}

const char *tracking_status(uint8_t fn)
{
    if (fn == MXP_FUNC_LINE) return "LINE";
    if (fn != MXP_FUNC_TRACK) return "?";
    if (!s_has_object) return "TRACK -";
    return s_found ? "TRACK" : "TRACK LOST";
}
