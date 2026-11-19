#ifdef MXCAM_HANDLM_BENCH
/* Hand landmark prototype, phase B: MediaPipe hand_landmark_lite (ESP-PPQ
 * .espdl variants from tools/handlm/quantize.py) on the board, in the normal
 * firmware (face + gesture models loaded, pipeline and encoder running).
 * Results: docs/research/hand-landmarks-proto-b.md. Never enabled in
 * committed builds / CI.
 *
 * Data comes from the `storage` partition (tools/handlm/pack_storage.py):
 * models at 64 KiB aligned offsets (the first at offset 0), an int16 input
 * crop ("HLIN"), RGB565BE test frames ("HLFR") and a directory ("HLDX") in the
 * last 64 KiB. Directory names ending in ":t" carry esp-ppq test values
 * (model->test()).
 *
 * Per model (one step per pipeline frame): load -> test() (":t" only) ->
 * 3 warm-up + 30 timed runs on the fixed crop (logs the outputs) -> per-module
 * profile (first cycle) -> end to end on each test frame
 * (hand_detect box -> square crop x1.3 -> run; presence < 0.9 -> also +-90
 * deg; MediaPipe rotated ROI from the best -> run ("acquired"); ROI from that
 * -> run ("tracking", what every later frame costs)) -> unload. Then the next
 * model; after the last one the cycle starts again.
 * The hand_detect boxes of the test frames are computed once at bench start;
 * then the gesture models are freed (mxh_deinit), because the int16-depthwise
 * variants do not fit in PSRAM next to them. All loads use the ADR-0011
 * malloc->PSRAM window. */
#include "handlm_bench.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <new>
#include <string>
#include <vector>

#include "dl_model_base.hpp"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_gesture.h"
#include "sdkconfig.h"

static const char *TAG = "hlm";

#define S 224
#define FW 320
#define FH 240
#define FRAME_PX (FW * FH)
#define MAX_ENTRIES 16
#define N_WARM 3
#define N_RUNS 30
#define HAND_THR 40 /* function 8 default hand_threshold */
#define MAX_HANDS 2
#define PRESENCE_RETRY 0.9f
#define BOX_SCALE 1.3f
#define START_DELAY_US (8 * 1000000LL)
#define CYCLE_PAUSE_US (5 * 1000000LL)
#define CRASH_MAGIC 0x484C4D42u

typedef struct {
    uint32_t off, bytes;
    char name[40];
} entry_t;

static entry_t s_dir[MAX_ENTRIES];
static int s_n_dir;
static const esp_partition_t *s_part;
static int s_input_idx = -1, s_frames_idx = -1, s_n_frames;
static std::vector<int> s_models;

/* A model whose load crashed the board (out of memory -> abort) is skipped
 * after the reboot, so the bench does not boot-loop. */
RTC_NOINIT_ATTR static uint32_t s_crash_magic;
RTC_NOINIT_ATTR static uint32_t s_crash_idx;
static int s_skip_idx = -1;

typedef struct {
    dl::Model *m;
    esp_partition_mmap_handle_t h;
    bool mapped;
    int idx;
} loaded_t;
static loaded_t s_cur = { nullptr, 0, false, -1 };

static bool has_test(int idx)
{
    size_t n = strlen(s_dir[idx].name);
    return n > 2 && strcmp(s_dir[idx].name + n - 2, ":t") == 0;
}

/* A ":t" model whose plain twin is also in storage: only test() + timing, the
 * profile and end-to-end runs are done on the twin (same numerics). */
static bool skip_e2e(int idx)
{
    if (!has_test(idx)) return false;
    size_t n = strlen(s_dir[idx].name) - 2;
    for (int i : s_models)
        if (i != idx && strlen(s_dir[i].name) == n && strncmp(s_dir[i].name, s_dir[idx].name, n) == 0) return true;
    return false;
}

static bool read_dir(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part) {
        ESP_LOGE(TAG, "no storage partition");
        return false;
    }
    uint32_t dir_off = s_part->size - 0x10000;
    char magic[4];
    uint32_t count = 0;
    if (esp_partition_read(s_part, dir_off, magic, 4) != ESP_OK || memcmp(magic, "HLDX", 4) != 0 ||
        esp_partition_read(s_part, dir_off + 4, &count, 4) != ESP_OK) {
        ESP_LOGE(TAG, "no HLDX directory in storage (write tools/handlm/pack_storage.py output)");
        return false;
    }
    s_n_dir = count > MAX_ENTRIES ? MAX_ENTRIES : (int)count;
    esp_partition_read(s_part, dir_off + 8, s_dir, sizeof(entry_t) * s_n_dir);
    for (int i = 0; i < s_n_dir; i++) {
        s_dir[i].name[39] = 0;
        esp_partition_read(s_part, s_dir[i].off, magic, 4);
        if (memcmp(magic, "EDL2", 4) == 0 || memcmp(magic, "EDL1", 4) == 0) {
            s_models.push_back(i);
        } else if (memcmp(magic, "HLIN", 4) == 0) {
            s_input_idx = i;
        } else if (memcmp(magic, "HLFR", 4) == 0) {
            s_frames_idx = i;
            uint32_t n = 0;
            esp_partition_read(s_part, s_dir[i].off + 4, &n, 4);
            s_n_frames = (int)n;
        }
        ESP_LOGI(TAG, "storage #%d 0x%06x %u B %.4s %s", i, (unsigned)s_dir[i].off, (unsigned)s_dir[i].bytes, magic,
                 s_dir[i].name);
    }
    return !s_models.empty();
}

static void log_mem(const char *what)
{
    ESP_LOGI(TAG, "%s: internal %u (min %u) | psram %u (min %u) block %u | stack_hw %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* window: steer plain malloc to PSRAM while loading (ADR-0011; boot only). */
static bool load_model(int idx, bool window)
{
    const entry_t &e = s_dir[idx];
    size_t in0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (idx == s_skip_idx) {
        ESP_LOGW(TAG, "load #%d %s: skipped (crashed the board last time)", idx, e.name);
        return false;
    }
    if (ps0 < e.bytes + 1024u * 1024u) {
        ESP_LOGW(TAG, "load #%d %s: skipped, psram free %u < model %u + 1 MiB", idx, e.name, (unsigned)ps0,
                 (unsigned)e.bytes);
        return false;
    }
    s_crash_magic = CRASH_MAGIC;
    s_crash_idx = (uint32_t)idx;
    loaded_t l = { nullptr, 0, false, idx };
    heap_caps_monitor_local_minimum_free_size_start();
    int64_t t0 = esp_timer_get_time();
#if CONFIG_SPIRAM_USE_MALLOC
    if (window) heap_caps_malloc_extmem_enable(0);
#endif
    const char *via;
    if (e.off == 0) {
        /* The real thing: esp-dl maps the partition itself (whole partition). */
        via = "partition";
        l.m = new (std::nothrow) dl::Model("storage", fbs::MODEL_LOCATION_IN_FLASH_PARTITION);
    } else {
        /* Other variants at an offset: map just that range, load as "rodata". */
        via = "mmap";
        const void *p = nullptr;
        esp_err_t err = esp_partition_mmap(s_part, e.off, e.bytes, ESP_PARTITION_MMAP_DATA, &p, &l.h);
        if (err == ESP_OK) {
            l.mapped = true;
            l.m = new (std::nothrow) dl::Model((const char *)p, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        } else {
            ESP_LOGE(TAG, "mmap #%d failed: %s", idx, esp_err_to_name(err));
        }
    }
#if CONFIG_SPIRAM_USE_MALLOC
    if (window) heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    int64_t t1 = esp_timer_get_time();
    size_t in_low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t ps_low = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    heap_caps_monitor_local_minimum_free_size_stop();
    s_crash_magic = 0;
    size_t in1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool built = l.m && l.m->is_built();
    ESP_LOGI(TAG,
             "load #%d %s: %s %u ms via %s window=%d | internal %u -> %u (used %d, lowest %u) | psram %u -> %u "
             "(used %d, lowest %u) block %u",
             idx, e.name, built ? "ok" : "FAILED", (unsigned)((t1 - t0) / 1000), via, window ? 1 : 0, (unsigned)in0,
             (unsigned)in1, (int)(in0 - in1), (unsigned)in_low, (unsigned)ps0, (unsigned)ps1, (int)(ps0 - ps1),
             (unsigned)ps_low, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if (!built) {
        delete l.m;
        if (l.mapped) esp_partition_munmap(l.h);
        return false;
    }
    std::map<std::string, dl::mem_info_t> mi = l.m->get_memory_info();
    for (auto &kv : mi)
        ESP_LOGI(TAG, "mem #%d %-14s psram %u internal %u flash %u", idx, kv.first.c_str(), (unsigned)kv.second.psram,
                 (unsigned)kv.second.internal, (unsigned)kv.second.flash);
    s_cur = l;
    return true;
}

static void unload_model(void)
{
    if (!s_cur.m) return;
    size_t in0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    delete s_cur.m;
    if (s_cur.mapped) esp_partition_munmap(s_cur.h);
    ESP_LOGI(TAG, "unload #%d: internal freed %d, psram freed %d", s_cur.idx,
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - in0),
             (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - ps0));
    s_cur = { nullptr, 0, false, -1 };
}

static dl::TensorBase *input_tensor(dl::Model *m) { return m->get_inputs().begin()->second; }

static dl::TensorBase *output(dl::Model *m, const char *name)
{
    std::map<std::string, dl::TensorBase *> &o = m->get_outputs();
    auto it = o.find(name);
    return it == o.end() ? nullptr : it->second;
}

static float deq(dl::TensorBase *t, int i)
{
    float sc = ldexpf(1.0f, t->get_exponent());
    switch (t->get_dtype()) {
    case dl::DATA_TYPE_INT8:
        return ((const int8_t *)t->data)[i] * sc;
    case dl::DATA_TYPE_INT16:
        return ((const int16_t *)t->data)[i] * sc;
    case dl::DATA_TYPE_FLOAT:
        return ((const float *)t->data)[i];
    default:
        return NAN;
    }
}

/* Outputs: landmarks [1,63] (x, y, z in crop px of 224), presence, handedness (sigmoids). */
static bool read_outputs(dl::Model *m, float lm[63], float *pres, float *hand)
{
    dl::TensorBase *l = output(m, "landmarks"), *p = output(m, "presence"), *h = output(m, "handedness");
    if (!l || !p || !h) return false;
    for (int i = 0; i < 63; i++) lm[i] = deq(l, i);
    *pres = deq(p, 0);
    *hand = deq(h, 0);
    return true;
}

/* ---------------------------------------------------------------- fixed crop */

static void *s_conv;   /* fixed crop, already in the model's input format */
static size_t s_conv_bytes;

static bool prep_input(dl::Model *m)
{
    dl::TensorBase *t = input_tensor(m);
    std::string shape;
    for (int d : t->shape) shape += std::to_string(d) + "x";
    ESP_LOGI(TAG, "input %s %s exp %d (%d elements)", shape.c_str(), t->get_dtype_string(), t->get_exponent(),
             t->get_size());
    if (s_input_idx < 0 || t->get_size() != S * S * 3) return false;
    s_conv_bytes = t->get_bytes();
    s_conv = heap_caps_malloc(s_conv_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_conv) return false;
    const void *p;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(s_part, s_dir[s_input_idx].off, s_dir[s_input_idx].bytes, ESP_PARTITION_MMAP_DATA, &p,
                           &h) != ESP_OK) {
        heap_caps_free(s_conv);
        s_conv = nullptr;
        return false;
    }
    const int16_t *src = (const int16_t *)((const uint8_t *)p + 20); /* HLIN header: 4s I I I i */
    int exp = t->get_exponent();
    for (int i = 0; i < S * S * 3; i++) {
        long q = lround(ldexp(src[i] / 32768.0, -exp));
        if (t->get_dtype() == dl::DATA_TYPE_INT8) {
            ((int8_t *)s_conv)[i] = (int8_t)(q > 127 ? 127 : (q < -128 ? -128 : q));
        } else {
            ((int16_t *)s_conv)[i] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
        }
    }
    esp_partition_munmap(h);
    return true;
}

/* ---------------------------------------------------------------- crops */

typedef struct {
    float cx, cy, side, ang; /* square in the image, rotated by ang (MediaPipe ROI convention) */
} rect_t;

/* crop px (qx, qy) -> image px; tools/handlm/hlm_common.py rect_affine inverse. */
static void crop_to_img(const rect_t &r, float qx, float qy, float *x, float *y)
{
    float k = r.side / S, c = cosf(r.ang) * k, s = sinf(r.ang) * k;
    float dx = qx - S / 2, dy = qy - S / 2;
    *x = r.cx + c * dx - s * dy;
    *y = r.cy + s * dx + c * dy;
}

static inline void rgb_at(const uint16_t *f, int x, int y, int rgb[3])
{
    if (x < 0 || y < 0 || x >= FW || y >= FH) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    uint16_t v = f[y * FW + x];
    v = (uint16_t)((v >> 8) | (v << 8)); /* big-endian RGB565 */
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

/* Bilinear affine crop of the frame straight into the model input (NHWC,
 * RGB 0..1 quantized with the input exponent; outside the frame = black),
 * like hlm_common.warp (PIL, pixel centres at +0.5, rounded to 8 bit). */
static void warp_to_input(const uint16_t *frame, const rect_t &r, dl::TensorBase *t)
{
    static int16_t lut[256];
    static int lut_exp = 1000;
    static dl::dtype_t lut_dt;
    int exp = t->get_exponent();
    bool i8 = t->get_dtype() == dl::DATA_TYPE_INT8;
    if (exp != lut_exp || t->get_dtype() != lut_dt) {
        long hi = i8 ? 127 : 32767;
        for (int i = 0; i < 256; i++) {
            long q = lround(ldexp(i / 255.0, -exp));
            lut[i] = (int16_t)(q > hi ? hi : q);
        }
        lut_exp = exp;
        lut_dt = t->get_dtype();
    }
    float k = r.side / S, c = cosf(r.ang) * k, s = sinf(r.ang) * k;
    int8_t *o8 = (int8_t *)t->data;
    int16_t *o16 = (int16_t *)t->data;
    int i = 0;
    for (int v = 0; v < S; v++) {
        float dy = v + 0.5f - S / 2;
        float x = r.cx + c * (0.5f - S / 2) - s * dy - 0.5f;
        float y = r.cy + s * (0.5f - S / 2) + c * dy - 0.5f;
        for (int u = 0; u < S; u++, x += c, y += s) {
            float fx0 = floorf(x), fy0 = floorf(y);
            int x0 = (int)fx0, y0 = (int)fy0;
            float ax = x - fx0, ay = y - fy0;
            int p00[3], p01[3], p10[3], p11[3];
            rgb_at(frame, x0, y0, p00);
            rgb_at(frame, x0 + 1, y0, p01);
            rgb_at(frame, x0, y0 + 1, p10);
            rgb_at(frame, x0 + 1, y0 + 1, p11);
            for (int ch = 0; ch < 3; ch++, i++) {
                float top = p00[ch] + (p01[ch] - p00[ch]) * ax;
                float bot = p10[ch] + (p11[ch] - p10[ch]) * ax;
                int val = (int)(top + (bot - top) * ay + 0.5f);
                val = val < 0 ? 0 : (val > 255 ? 255 : val);
                if (i8)
                    o8[i] = (int8_t)lut[val];
                else
                    o16[i] = lut[val];
            }
        }
    }
}

/* MediaPipe hand_landmark_landmarks_to_roi (hlm_common.mediapipe_roi_from_landmarks). */
static rect_t roi_from_landmarks(const float lm[21][2])
{
    static const int idx[12] = { 0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18 };
    float x0 = lm[0][0], y0 = lm[0][1], x1 = lm[9][0], y1 = lm[9][1];
    float rot = (float)M_PI / 2 - atan2f(-(y1 - y0), x1 - x0);
    rot = rot - 2 * (float)M_PI * floorf((rot + (float)M_PI) / (2 * (float)M_PI));
    float c = cosf(rot), s = sinf(rot);
    float rxmin = 1e9f, rxmax = -1e9f, rymin = 1e9f, rymax = -1e9f;
    for (int k = 0; k < 12; k++) {
        float dx = lm[idx[k]][0] - x0, dy = lm[idx[k]][1] - y0;
        float rx = dx * c + dy * s, ry = -dx * s + dy * c;
        rxmin = fminf(rxmin, rx);
        rxmax = fmaxf(rxmax, rx);
        rymin = fminf(rymin, ry);
        rymax = fmaxf(rymax, ry);
    }
    float cxr = (rxmin + rxmax) / 2, cyr = (rymin + rymax) / 2, w = rxmax - rxmin, h = rymax - rymin;
    rect_t r;
    r.cx = x0 + cxr * c - cyr * s;
    r.cy = y0 + cxr * s + cyr * c;
    float shift = -0.1f * h;
    r.cx += -shift * s;
    r.cy += shift * c;
    r.side = fmaxf(w, h) * 2.0f;
    r.ang = rot;
    return r;
}

typedef struct {
    float lm[21][2]; /* image px */
    float pres, hand;
    uint32_t warp_us, run_us;
} pass_t;

static bool infer_rect(dl::Model *m, const uint16_t *frame, const rect_t &r, pass_t *o)
{
    dl::TensorBase *t = input_tensor(m);
    int64_t t0 = esp_timer_get_time();
    warp_to_input(frame, r, t);
    int64_t t1 = esp_timer_get_time();
    m->run();
    int64_t t2 = esp_timer_get_time();
    float raw[63];
    if (!read_outputs(m, raw, &o->pres, &o->hand)) return false;
    for (int k = 0; k < 21; k++) crop_to_img(r, raw[3 * k], raw[3 * k + 1], &o->lm[k][0], &o->lm[k][1]);
    o->warp_us = (uint32_t)(t1 - t0);
    o->run_us = (uint32_t)(t2 - t1);
    return true;
}

static void log_lm(const char *what, int cyc, int fi, int hi, const pass_t &p)
{
    char buf[21 * 18 + 1];
    int len = 0;
    for (int k = 0; k < 21; k++)
        len += snprintf(buf + len, sizeof(buf) - len, " %.1f,%.1f", p.lm[k][0], p.lm[k][1]);
    ESP_LOGI(TAG, "%s c%d #%d f%d h%d p=%.3f hd=%.3f:%s", what, cyc, s_cur.idx, fi, hi, p.pres, p.hand, buf);
}

/* One hand from a hand_detect box: acquisition (+ rotation retries), refine, one tracking pass. */
static void track_hand(dl::Model *m, int cyc, int fi, int hi, const uint16_t *frame, const mxp_result_t &box)
{
    rect_t r0 = { (float)box.x, (float)box.y, fmaxf((float)box.w, (float)box.h) * BOX_SCALE, 0.0f };
    pass_t p[3], acq, trk;
    int64_t t0 = esp_timer_get_time();
    if (!infer_rect(m, frame, r0, &p[0])) return;
    int passes = 1, best = 0;
    uint32_t warp = p[0].warp_us, run = p[0].run_us;
    if (p[0].pres < PRESENCE_RETRY) {
        for (int k = 1; k <= 2; k++) {
            rect_t rr = r0;
            rr.ang = (k == 1 ? 1 : -1) * (float)M_PI / 2;
            infer_rect(m, frame, rr, &p[k]);
            warp += p[k].warp_us;
            run += p[k].run_us;
            passes++;
            if (p[k].pres > p[best].pres) best = k;
        }
    }
    infer_rect(m, frame, roi_from_landmarks(p[best].lm), &acq);
    passes++;
    warp += acq.warp_us;
    run += acq.run_us;
    uint32_t acq_us = (uint32_t)(esp_timer_get_time() - t0);
    int64_t t1 = esp_timer_get_time();
    infer_rect(m, frame, roi_from_landmarks(acq.lm), &trk);
    uint32_t trk_us = (uint32_t)(esp_timer_get_time() - t1);
    ESP_LOGI(TAG,
             "e2e c%d #%d f%d h%d box=%d,%d %ux%u id=%u conf=%u | acq passes=%d %u ms (run %u, warp %u) p0=%.3f "
             "p90=%.3f pm90=%.3f best=%d pA=%.3f | track %u ms (run %u, warp %u) pT=%.3f",
             cyc, s_cur.idx, fi, hi, box.x, box.y, box.w, box.h, box.id, box.confidence, passes,
             (unsigned)(acq_us / 1000), (unsigned)(run / 1000), (unsigned)(warp / 1000), p[0].pres,
             passes > 2 ? p[1].pres : -1.0f, passes > 2 ? p[2].pres : -1.0f, best, acq.pres,
             (unsigned)(trk_us / 1000), (unsigned)(trk.run_us / 1000), (unsigned)(trk.warp_us / 1000), trk.pres);
    log_lm("lmA", cyc, fi, hi, acq);
    log_lm("lmT", cyc, fi, hi, trk);
}

/* ---------------------------------------------------------------- state machine */

enum { ST_START, ST_LOAD, ST_TEST, ST_PREP, ST_RUN, ST_PROFILE, ST_E2E, ST_UNLOAD, ST_PAUSE, ST_OFF };
static int s_st = ST_OFF, s_vi, s_k, s_cycle, s_fi, s_hi;
static int64_t s_wait_until;
static uint32_t s_times[N_WARM + N_RUNS];
static size_t s_run_in_low, s_run_ps_low;
#define MAX_FRAMES 8
static mxp_result_t s_boxes[MAX_FRAMES][MAX_HANDS]; /* hand_detect boxes of the test frames */
static int s_fnh[MAX_FRAMES];
static uint16_t *s_frame; /* test frame copied to PSRAM (like a camera buffer) */

extern "C" void handlm_bench_boot(void)
{
    if (s_crash_magic == CRASH_MAGIC) {
        esp_reset_reason_t why = esp_reset_reason();
        if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT) s_skip_idx = (int)s_crash_idx;
        ESP_LOGW(TAG, "previous run crashed while loading #%u (reset reason %d)", (unsigned)s_crash_idx, (int)why);
    }
    s_crash_magic = 0;
    if (!read_dir()) return;
#ifdef MXCAM_HANDLM_BENCH_ONLY
    /* Only this directory entry: load it at boot, measure it once, stop. */
    s_models.assign(1, MXCAM_HANDLM_BENCH_ONLY);
#endif
    log_mem("before boot load");
    load_model(s_models[0], true);
    s_st = ST_START;
    s_wait_until = esp_timer_get_time() + START_DELAY_US;
}

static void next_model(void)
{
    if (s_conv) heap_caps_free(s_conv);
    s_conv = nullptr;
    unload_model();
    s_vi++;
    if (s_vi >= (int)s_models.size()) {
        ESP_LOGI(TAG, "cycle %d done", s_cycle);
        log_mem("cycle end");
#ifdef MXCAM_HANDLM_BENCH_ONLY
        s_st = ST_OFF;
        return;
#endif
        s_cycle++;
        s_vi = 0;
        s_st = ST_PAUSE;
        s_wait_until = esp_timer_get_time() + CYCLE_PAUSE_US;
    } else {
        s_st = ST_LOAD;
    }
}

static bool read_frame(int fi)
{
    if (!s_frame) s_frame = (uint16_t *)heap_caps_malloc(FRAME_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) return false;
    return esp_partition_read(s_part, s_dir[s_frames_idx].off + 16 + (uint32_t)fi * FRAME_PX * 2, s_frame,
                              FRAME_PX * 2) == ESP_OK;
}

/* Bench start (gesture models still loaded): hand_detect boxes of every test
 * frame, then free the gesture models so the landmark variants fit (the
 * dw16 variants need ~3.8-4.4 MB of PSRAM; see the doc). */
/* hand_detect boxes of the 8 packed test frames (batch1 + batch2) as measured
 * on the board (hand_threshold 40), for builds where the gesture models are
 * not loaded (MXCAM_HANDLM_BENCH_EARLY): {x, y, w, h, id, conf}. */
static const int16_t k_boxes[MAX_FRAMES][MAX_HANDS][6] = {
    { { 91, 95, 39, 77, 1, 100 }, {} },
    { { 235, 121, 63, 127, 2, 100 }, { 81, 118, 66, 120, 1, 100 } },
    { { 242, 123, 96, 144, 4, 100 }, { 75, 122, 60, 115, 3, 100 } },
    { { 77, 118, 125, 128, 5, 100 }, { 235, 123, 77, 116, 6, 100 } },
    { { 72, 124, 84, 115, 7, 97 }, { 249, 116, 111, 116, 8, 100 } },
    { { 173, 134, 103, 148, 9, 100 }, {} },
    { { 61, 160, 30, 20, 0, 73 }, { 98, 126, 17, 19, 0, 68 } },
    { { 283, 126, 72, 170, 0, 88 }, {} },
};
static const int k_nboxes[MAX_FRAMES] = { 1, 2, 2, 2, 2, 1, 2, 1 };

static void detect_frames(void)
{
    int n_frames = s_n_frames > MAX_FRAMES ? MAX_FRAMES : s_n_frames;
#ifdef MXCAM_HANDLM_BENCH_EARLY
    {
        ESP_LOGI(TAG, "early build: using the recorded hand_detect boxes");
        mxh_deinit(); /* if mxh_init still fitted after the landmark model */
        log_mem("gesture models freed");
        for (int fi = 0; fi < n_frames; fi++) {
            s_fnh[fi] = k_nboxes[fi];
            for (int h = 0; h < k_nboxes[fi]; h++) {
                mxp_result_t &b = s_boxes[fi][h];
                memset(&b, 0, sizeof(b));
                b.x = k_boxes[fi][h][0];
                b.y = k_boxes[fi][h][1];
                b.w = (uint16_t)k_boxes[fi][h][2];
                b.h = (uint16_t)k_boxes[fi][h][3];
                b.id = (uint16_t)k_boxes[fi][h][4];
                b.confidence = (uint8_t)k_boxes[fi][h][5];
            }
        }
        s_n_frames = n_frames;
        return;
    }
#endif
    for (int fi = 0; fi < n_frames; fi++) {
        mxh_stats_t st = {};
        int n = read_frame(fi) ? mxh_detect(s_frame, HAND_THR, 0, MAX_HANDS, s_boxes[fi], MAX_HANDS, &st) : -1;
        s_fnh[fi] = n < 0 ? 0 : n;
        ESP_LOGI(TAG, "det f%d: %d hand(s) in %u ms", fi, n, (unsigned)(st.detect_us / 1000));
        for (int h = 0; h < s_fnh[fi]; h++)
            ESP_LOGI(TAG, "det f%d h%d: box=%d,%d %ux%u id=%u conf=%u", fi, h, s_boxes[fi][h].x, s_boxes[fi][h].y,
                     s_boxes[fi][h].w, s_boxes[fi][h].h, s_boxes[fi][h].id, s_boxes[fi][h].confidence);
    }
    s_n_frames = n_frames;
    if (s_frame) heap_caps_free(s_frame);
    s_frame = nullptr;
    log_mem("before freeing the gesture models");
    mxh_deinit();
    log_mem("gesture models freed");
}

/* One hand of one test frame per call. */
static void e2e_step(void)
{
    while (s_fi < s_n_frames && s_hi >= s_fnh[s_fi]) {
        s_fi++;
        s_hi = 0;
    }
    if (s_fi >= s_n_frames) {
        s_st = ST_UNLOAD;
        return;
    }
    if (s_hi == 0 && !read_frame(s_fi)) {
        s_st = ST_UNLOAD;
        return;
    }
    track_hand(s_cur.m, s_cycle, s_fi, s_hi, s_frame, s_boxes[s_fi][s_hi]);
    s_hi++;
}

extern "C" void handlm_bench_frame(const uint16_t *frame)
{
    (void)frame;
    int64_t now = esp_timer_get_time();
    switch (s_st) {
    case ST_OFF:
        return;
    case ST_START:
    case ST_PAUSE:
        if (now < s_wait_until) return;
        if (s_st == ST_START) {
            log_mem("bench start");
            detect_frames();
        } else {
            log_mem("cycle start");
        }
        s_st = (s_cur.m && s_cur.idx == s_models[s_vi]) ? ST_TEST : ST_LOAD;
        return;
    case ST_LOAD:
        /* Inside the malloc->PSRAM window too: without it (first try) a
         * load kept 50-64 KB of internal RAM and drained it to < 100 B
         * while loading (heap min 16 B). */
        if (!load_model(s_models[s_vi], true))
            next_model();
        else
            s_st = ST_TEST;
        return;
    case ST_TEST:
        if (has_test(s_cur.idx)) {
            int64_t t0 = esp_timer_get_time();
            esp_err_t r = s_cur.m->test();
            ESP_LOGI(TAG, "test c%d #%d %s: %s (%u ms)", s_cycle, s_cur.idx, s_dir[s_cur.idx].name,
                     r == ESP_OK ? "PASS" : "FAIL", (unsigned)((esp_timer_get_time() - t0) / 1000));
        }
        s_st = ST_PREP;
        return;
    case ST_PREP:
        if (!prep_input(s_cur.m)) {
            ESP_LOGE(TAG, "no fixed input");
            s_st = ST_UNLOAD;
            return;
        }
        s_k = 0;
        s_run_in_low = SIZE_MAX;
        s_run_ps_low = SIZE_MAX;
        s_st = ST_RUN;
        return;
    case ST_RUN: {
        if (s_k == 0) log_mem("first run");
        dl::TensorBase *t = input_tensor(s_cur.m);
        memcpy(t->data, s_conv, s_conv_bytes);
        heap_caps_monitor_local_minimum_free_size_start();
        int64_t t0 = esp_timer_get_time();
        s_cur.m->run();
        s_times[s_k++] = (uint32_t)(esp_timer_get_time() - t0);
        size_t il = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t pl = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        heap_caps_monitor_local_minimum_free_size_stop();
        if (il < s_run_in_low) s_run_in_low = il;
        if (pl < s_run_ps_low) s_run_ps_low = pl;
        if (s_k < N_WARM + N_RUNS) return;
        uint64_t sum = 0;
        uint32_t mx = 0, mn = UINT32_MAX;
        char all[N_RUNS * 5 + 1];
        int len = 0;
        for (int i = N_WARM; i < N_WARM + N_RUNS; i++) {
            sum += s_times[i];
            if (s_times[i] > mx) mx = s_times[i];
            if (s_times[i] < mn) mn = s_times[i];
            len += snprintf(all + len, sizeof(all) - len, " %u", (unsigned)(s_times[i] / 1000));
        }
        ESP_LOGI(TAG,
                 "run c%d #%d %s: avg %.1f min %.1f max %.1f ms over %d (warm-up %u %u %u) | lowest in run: "
                 "internal %u psram %u | stack_hw %u",
                 s_cycle, s_cur.idx, s_dir[s_cur.idx].name, sum / 1000.0 / N_RUNS, mn / 1000.0, mx / 1000.0, N_RUNS,
                 (unsigned)(s_times[0] / 1000), (unsigned)(s_times[1] / 1000), (unsigned)(s_times[2] / 1000),
                 (unsigned)s_run_in_low, (unsigned)s_run_ps_low, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "runs c%d #%d ms:%s", s_cycle, s_cur.idx, all);
        float lm[63], pres, hand;
        if (read_outputs(s_cur.m, lm, &pres, &hand)) {
            char buf[21 * 18 + 1];
            int l2 = 0;
            for (int k = 0; k < 21; k++) l2 += snprintf(buf + l2, sizeof(buf) - l2, " %.2f,%.2f", lm[3 * k], lm[3 * k + 1]);
            ESP_LOGI(TAG, "out c%d #%d p=%.4f hd=%.4f:%s", s_cycle, s_cur.idx, pres, hand, buf);
        }
        s_st = skip_e2e(s_cur.idx) ? ST_UNLOAD : (s_cycle == 0 ? ST_PROFILE : ST_E2E);
        s_fi = s_hi = 0;
        return;
    }
    case ST_PROFILE: {
        dl::TensorBase *t = input_tensor(s_cur.m);
        memcpy(t->data, s_conv, s_conv_bytes);
        std::map<std::string, dl::module_info> info = s_cur.m->get_module_info();
        for (auto &kv : info)
            printf("prof #%d|%s|%s|%u\n", s_cur.idx, kv.first.c_str(), kv.second.type.c_str(),
                   (unsigned)kv.second.latency);
        s_st = ST_E2E;
        return;
    }
    case ST_E2E:
        e2e_step();
        return;
    case ST_UNLOAD:
        if (s_frame) heap_caps_free(s_frame);
        s_frame = nullptr;
        next_model();
        return;
    }
}
#endif
