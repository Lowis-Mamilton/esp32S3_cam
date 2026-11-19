#include "pipeline.h"

#include <stdio.h>
#include <string.h>

#include "encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_bench.h" /* empty unless MXCAM_GESTURE_BENCH */
#include "handlm_bench.h"  /* empty unless MXCAM_HANDLM_BENCH */
#include "mxcam_camera.h"
#include "mxcam_color.h"
#include "mxcam_display.h"
#include "mxcam_face.h"
#include "mxcam_gesture.h"
#include "mxcam_link.h"
#include "mxcam_overlay.h"
#include "mxcam_store.h"
#include "mxcam_tag.h"
#include "status_led.h"

static const char *TAG = "pipeline";

/* AprilTag (upstream apriltag, quad threshold + decode) needs a deep stack;
 * colour detection keeps ~3 KB of HSV tables on it. The 1 Hz log prints the
 * high-water mark. */
#define PIPE_STACK (24 * 1024)
#define PIPE_PRIO 5
#define ICON_SIZE 12
#define ICON_MARGIN 4
#define BOX_THICKNESS 2

/* Studio's ID palette (pc-app/src/theme.css --id-1..--id-8) as RGB565 in the
 * panel's big-endian byte order. */
#define HEX565(rgb) ((uint16_t)(((((rgb) >> 19) & 0x1F) << 11) | ((((rgb) >> 10) & 0x3F) << 5) | (((rgb) >> 3) & 0x1F)))
#define ID_COLOR(rgb) MXO_RGB565(HEX565(rgb))
static const uint16_t k_id_colors[8] = {
    ID_COLOR(0xef4b4b), ID_COLOR(0xa3d62f), ID_COLOR(0x34d27b), ID_COLOR(0x2cc8d8),
    ID_COLOR(0x4d8ef5), ID_COLOR(0x8f6df2), ID_COLOR(0xd45be0), ID_COLOR(0xf2609b),
};

/* Face boxes use the 8th ID colour (Studio --id-8). */
#define FACE_COLOR k_id_colors[7]

/* Face detection takes 44-62 ms per frame (M3 measurements). It runs on
 * every FACE_EVERY-th frame; the frames in between show the last detection
 * on the TFT and do not cache or push results (those are per detection).
 * 1 = every frame: measured TFT 10-11 fps without streaming, 7 fps while
 * streaming (docs/SPEC.md 6.1); 2 would only be needed below 7 fps. */
#define FACE_EVERY 1

/* Function name for the TFT's top-left text and the 1 Hz log (ASCII). */
static const char *fn_name(uint8_t fn)
{
    switch (fn) {
    case MXP_FUNC_NONE: return "NONE";
    case MXP_FUNC_COLOR: return "COLOR";
    case MXP_FUNC_APRILTAG: return "TAG";
    case MXP_FUNC_FACE: return "FACE";
    case MXP_FUNC_GESTURE: return "GESTURE";
    default: return "?";
    }
}

#ifdef MXCAM_TAG_BENCH
/* Optional on-board benchmark (off by default; build with
 * -DMXCAM_TAG_BENCH, e.g. target_compile_definitions in main/CMakeLists.txt):
 * in TAG mode it cycles through detector variants every BENCH_MS and logs one
 * "bench" line per variant (TFT fps, stage times, internal heap minimum).
 * Used for the Task 6 measurements; it changes the detector settings, so
 * never ship it enabled. */
#include "mxt_alloc.h"
#define BENCH_MS 8000
typedef struct {
    const char *name;
    uint8_t half;
    float dec;
    uint8_t psram;
    uint8_t wb;
    uint8_t refine;
    uint8_t minpix;
    uint8_t nmax;
    uint8_t mse;
} bench_t;
static const bench_t k_bench[] = {
    { "d2 wb20 (default)", 0, 2, 1, 20, 1, 5, 10, 10 },
    { "d2 wb30", 0, 2, 1, 30, 1, 5, 10, 10 },
    { "d3 wb20", 0, 3, 1, 20, 1, 5, 10, 10 },
    { "half wb20", 1, 1, 1, 20, 1, 5, 10, 10 },
    { "d2 wb5 (upstream)", 0, 2, 1, 5, 1, 5, 10, 10 },
};
#define N_BENCH (int)(sizeof(k_bench) / sizeof(k_bench[0]))
static int s_bi = -1;
static int64_t s_bstart;
static unsigned s_bframes;
static uint64_t s_bsum;
static uint32_t s_bmax;
static uint64_t s_bst[9];

static void bench_frame_start(void)
{
    int64_t now = esp_timer_get_time();
    if (s_bi >= 0 && now - s_bstart < BENCH_MS * 1000LL) return;
    if (s_bi >= 0 && s_bframes > 0) {
        double secs = (double)(now - s_bstart) / 1e6;
        unsigned f = s_bframes;
        ESP_LOGI("bench", "[%s] tft_fps=%.1f proc_ms=%u/%u gray=%u dec=%u thr=%u uf=%u clu=%u fit=%u decode=%u oth=%u nqx10=%u imin=%u",
                 k_bench[s_bi].name, f / secs, (unsigned)(s_bsum / f / 1000), (unsigned)(s_bmax / 1000),
                 (unsigned)(s_bst[0] / f), (unsigned)(s_bst[1] / f), (unsigned)(s_bst[2] / f), (unsigned)(s_bst[3] / f),
                 (unsigned)(s_bst[4] / f), (unsigned)(s_bst[5] / f), (unsigned)(s_bst[6] / f), (unsigned)(s_bst[7] / f),
                 (unsigned)(s_bst[8] * 10 / f), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    s_bi = (s_bi + 1) % N_BENCH;
    const bench_t *b = &k_bench[s_bi];
    mxt_alloc_set_mode(b->psram ? MXT_ALLOC_PSRAM : MXT_ALLOC_DEFAULT);
    mxt_set_quad_decimate(b->dec);
    mxt_tuning_t t;
    mxt_get_tuning(&t);
    t.half_res = b->half;
    t.min_white_black_diff = b->wb;
    t.refine_edges = b->refine;
    t.min_cluster_pixels = b->minpix;
    t.max_nmaxima = b->nmax;
    t.max_line_fit_mse = b->mse;
    mxt_set_tuning(&t);
    heap_caps_monitor_local_minimum_free_size_start();
    s_bstart = now;
    s_bframes = 0;
    s_bsum = 0;
    s_bmax = 0;
    memset(s_bst, 0, sizeof(s_bst));
}

static void bench_frame_end(uint32_t proc_us)
{
    mxt_profile_t p;
    mxt_last_profile(&p);
    s_bframes++;
    s_bsum += proc_us;
    if (proc_us > s_bmax) s_bmax = proc_us;
    uint32_t v[9] = { p.gray_us, p.decimate_us, p.threshold_us, p.unionfind_us, p.clusters_us,
                      p.fitquads_us, p.decode_us, p.other_us, p.nquads };
    for (int i = 0; i < 9; i++) s_bst[i] += v[i];
}
#endif

#ifdef MXCAM_FACE_BENCH
/* Optional on-board face detection benchmark (off by default; local builds
 * only: idf.py -DMXCAM_FACE_BENCH=ON build, see main/CMakeLists.txt). Loads
 * the face models on the pipeline task (logging the time, memory and stack
 * that takes), then once per second runs mxf_detect on the live frame,
 * whatever the current function, and logs one "face_bench" line (time,
 * results, internal heap free / lowest during the call / min, PSRAM free,
 * stack high-water mark). With -DMXCAM_FACE_BENCH_IMAGE=<file> it also runs on
 * each embedded 320x240 RGB565BE test frame. Used for the M3 Task 1
 * measurements; it stalls the TFT on those frames, so never ship it enabled.
 * Note: app_main already calls mxf_init (idempotent), so the init time and
 * memory numbers logged here are only meaningful if that main.c call is
 * disabled. */
#include "mxcam_face.h"
#define FB_FRAME_BYTES ((size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2)
#define FB_MAX_IMAGES 4
#ifdef MXCAM_FACE_BENCH_IMAGE
extern const uint8_t face_bench_img_start[] asm("_binary_face_bench_img_start");
extern const uint8_t face_bench_img_end[] asm("_binary_face_bench_img_end");
#endif
typedef struct {
    unsigned runs;
    uint64_t sum_us;
    uint32_t max_us;
} fb_stats_t;

static void face_bench_run(const char *label, const uint16_t *frame, const mxp_config_t *cfg, fb_stats_t *st)
{
    mxp_result_t r[MXP_MAX_RESULTS];
    uint32_t us = 0;
    heap_caps_monitor_local_minimum_free_size_start();
    uint8_t n = mxf_detect(frame, cfg->face_threshold, cfg->face_max, r, MXP_MAX_RESULTS, &us);
    size_t lmin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    st->runs++;
    st->sum_us += us;
    if (us > st->max_us) st->max_us = us;
    char faces[96] = "";
    int len = 0;
    for (int i = 0; i < n && i < 3 && len < (int)sizeof(faces); i++)
        len += snprintf(faces + len, sizeof(faces) - (size_t)len, " [%u %d,%d %ux%u]", r[i].confidence, r[i].x,
                        r[i].y, r[i].w, r[i].h);
    ESP_LOGI("face_bench",
             "%s ms=%u avg=%u max=%u n=%u thr=%u max_res=%u%s | internal %u (lowest in call %u) min %u | psram %u | "
             "stack_hw %u",
             label, (unsigned)(us / 1000), (unsigned)(st->sum_us / st->runs / 1000), (unsigned)(st->max_us / 1000), n,
             cfg->face_threshold, cfg->face_max, faces, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)lmin, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void face_bench(const uint16_t *frame, const mxp_config_t *cfg)
{
    static bool tried, ok;
    static int64_t next_us;
    static fb_stats_t st[1 + FB_MAX_IMAGES];
    static uint16_t *imgs; /* test frames copied to PSRAM, like camera buffers */
    static int n_imgs;
    int64_t now = esp_timer_get_time();
    if (!tried) {
        tried = true;
        size_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t p0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        unsigned s0 = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        size_t gmin0 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        heap_caps_monitor_local_minimum_free_size_start();
        ok = mxf_init();
        size_t lmin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        heap_caps_monitor_local_minimum_free_size_stop();
        ESP_LOGI("face_bench",
                 "init=%d %u ms | internal %u -> %u (lowest during init %u, min before %u) | psram %u -> %u | "
                 "stack_hw %u -> %u",
                 ok ? 1 : 0, (unsigned)((esp_timer_get_time() - now) / 1000), (unsigned)i0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)lmin, (unsigned)gmin0, (unsigned)p0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), s0, (unsigned)uxTaskGetStackHighWaterMark(NULL));
#ifdef MXCAM_FACE_BENCH_IMAGE
        size_t bytes = (size_t)(face_bench_img_end - face_bench_img_start);
        n_imgs = (int)(bytes / FB_FRAME_BYTES);
        if (n_imgs > FB_MAX_IMAGES) n_imgs = FB_MAX_IMAGES;
        if (n_imgs > 0) imgs = heap_caps_malloc(n_imgs * FB_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (imgs) memcpy(imgs, face_bench_img_start, n_imgs * FB_FRAME_BYTES);
        else n_imgs = 0;
        ESP_LOGI("face_bench", "%d test frame(s) from %u bytes", n_imgs, (unsigned)bytes);
#endif
        next_us = now + 1000000;
        return;
    }
    if (!ok || now < next_us) return;
    next_us = now + 1000000;
    face_bench_run("live", frame, cfg, &st[0]);
    for (int i = 0; i < n_imgs; i++) {
        char label[16];
        snprintf(label, sizeof(label), "img%d", i);
        face_bench_run(label, imgs + (size_t)i * MXCAM_LCD_W * MXCAM_LCD_H, cfg, &st[1 + i]);
    }
}
#endif

#ifdef MXCAM_JPEG_SELFTEST
/* Colour self-test (debug builds only, -DMXCAM_JPEG_SELFTEST): FRAME pushes
 * with frame_id % 4 == 0 or 1 carry a synthetic pattern instead of the camera
 * image (4 vertical bars: red F800, green 07E0, blue 001F, white FFFF, stored
 * big-endian like the camera); the encoder encodes % 4 == 1 as RGB565_LE.
 * Decode the JPEGs on the PC to see which input format keeps the colours. */
static const uint16_t *selftest_frame(void)
{
    static uint16_t *buf;
    if (!buf) {
        buf = heap_caps_malloc((size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) return NULL;
        static const uint16_t bars[4] = { MXO_RGB565(0xF800), MXO_RGB565(0x07E0), MXO_RGB565(0x001F), MXO_RGB565(0xFFFF) };
        for (int y = 0; y < MXCAM_LCD_H; y++)
            for (int x = 0; x < MXCAM_LCD_W; x++) buf[y * MXCAM_LCD_W + x] = bars[x / (MXCAM_LCD_W / 4)];
    }
    return buf;
}
#endif

static mxs_state_t *s_state;
static const char *s_boot_error;

/* Per-frame copy of what the pipeline needs from the shared state. */
typedef struct {
    uint32_t now_ms;
    bool connected, want_frames, streaming;
    uint8_t fps, quality, fn;
    mxp_config_t cfg;
    mxp_gesture_config_t gesture;
    mxc_learned_t colors[MXST_MAX_COLORS];
    uint8_t n_colors;
} frame_ctx_t;

static void copy_colors(frame_ctx_t *c)
{
    c->n_colors = s_state->data->n_colors;
    memcpy(c->colors, s_state->data->colors, sizeof(c->colors));
}

/* Boxes + ASCII labels ("ID1" / "T3" / "FACE 92" / "LIKE 92") above them
 * (inside near the top edge when there is no room above). Colour id 0 (not
 * learned) is never drawn; gesture id 0 (hand, no gesture) is white. */
static void draw_results(mxo_canvas_t *c, const mxp_results_t *res)
{
    for (int i = 0; i < res->count; i++) {
        const mxp_result_t *r = &res->r[i];
        uint16_t col;
        char label[12];
        if (res->function_id == MXP_FUNC_COLOR) {
            if (r->id < 1 || r->id > MXST_MAX_COLORS) continue;
            col = k_id_colors[r->id - 1];
            snprintf(label, sizeof(label), "ID%u", (unsigned)r->id);
        } else if (res->function_id == MXP_FUNC_APRILTAG) {
            col = k_id_colors[r->id % 7]; /* id % 7 + 1 -> palette index id % 7 */
            snprintf(label, sizeof(label), "T%u", (unsigned)r->id);
        } else if (res->function_id == MXP_FUNC_FACE) {
            col = FACE_COLOR;
            snprintf(label, sizeof(label), "FACE %u", (unsigned)r->confidence);
        } else if (res->function_id == MXP_FUNC_GESTURE) {
            col = r->id >= 1 && r->id <= MXP_GESTURE_MAX ? k_id_colors[(r->id - 1) % 8] : MXO_WHITE;
            mxh_label(r->id, r->confidence, label, sizeof(label));
        } else {
            continue;
        }
        /* The box is drawn in frame (true image) coordinates; the panel
         * mirrors it together with the image. The label is placed in panel
         * coordinates (above the box's left edge as seen on the panel) and
         * drawn mirrored so it reads correctly (ADR-0010). */
        int x0 = r->x - r->w / 2, y0 = r->y - r->h / 2;
        mxo_draw_rect(c, x0, y0, r->w, r->h, BOX_THICKNESS, col);
        int tw = (int)strlen(label) * 8;
        int lx = mxo_mirror_x(c, x0, r->w); /* box left edge on the panel */
        if (lx + tw > c->width) lx = c->width - tw;
        if (lx < 0) lx = 0;
        int ly = y0 - 9;
        if (ly < 0) ly = y0 + BOX_THICKNESS + 1;
        mxo_draw_text_mirrored(c, lx, ly, label, MXO_BLACK, col);
    }
}

static void pipeline_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    int frames = 0, fps = 0;
    /* Cumulative error counters, logged once per second. */
    unsigned grab_fail = 0, bad_size = 0, lcd_err = 0, results_push_fail = 0, stale_drop = 0;
    uint64_t tag_sum[5] = { 0 }; /* gray, decimate+threshold, unionfind+clusters, fit, decode */
    unsigned tag_frames = 0;
    uint64_t proc_sum_us = 0;
    uint32_t proc_max_us = 0;
    /* Face: per-second detection time stats; the last detection, shown on
     * the frames skipped by FACE_EVERY. */
    uint64_t face_sum_us = 0;
    uint32_t face_max_us = 0;
    unsigned face_runs = 0, face_phase = 0;
    static mxp_result_t face_last[MXP_MAX_RESULTS];
    uint8_t face_last_n = 0;
    /* Gesture: per-second recognition stats (every frame, ADR-0019); the
     * tracked hand boxes reused between detections (ADR-0019 addendum). */
    uint64_t gest_sum_us = 0, gest_det_us = 0, gest_cls_us = 0;
    uint32_t gest_max_us = 0;
    unsigned gest_runs = 0, gest_skip = 0, gest_dets = 0, gest_lost = 0;
    static mxh_track_t gest_trk;
    uint8_t last_count = 0, last_fn = 0;
    uint32_t frame_id = 1; /* 0 means "no frame processed yet" in GET_RESULTS */
    /* Streaming schedule: next_due_ms advances by one interval per submitted
     * frame (not "now + interval") so the average rate matches max_fps even
     * when the frame period does not divide the interval. */
    uint32_t next_due_ms = 0;
    bool was_wanting = false, streaming_now = false;
    static frame_ctx_t ctx;
    static mxp_results_t res;
    static uint8_t packed[MXS_RESULTS_MAX];

#ifdef MXCAM_GESTURE_BENCH
    /* Where a frame's time goes besides recognition (gesture bench builds). */
    uint64_t bench_grab_us = 0, bench_lcd_us = 0, bench_feed_us = 0;
#endif
    for (;;) {
#ifdef MXCAM_GESTURE_BENCH
        int64_t bench_t = esp_timer_get_time();
#endif
        camera_fb_t *fb = mxcam_camera_grab();
#ifdef MXCAM_GESTURE_BENCH
        bench_grab_us += (uint64_t)(esp_timer_get_time() - bench_t);
#endif
        if (!fb || fb->width != MXCAM_LCD_W || fb->height != MXCAM_LCD_H ||
            fb->len < (size_t)MXCAM_LCD_W * MXCAM_LCD_H * 2) {
            /* mxcam_display_draw and the detectors assume a full 320x240 RGB565 buffer. */
            if (!fb) grab_fail++;
            else bad_size++;
            mxcam_camera_release(fb);
            /* Keep enforcing the stream timeout while the camera fails. */
            mxcam_link_state_lock();
            mxs_tick(s_state, (uint32_t)(esp_timer_get_time() / 1000));
            mxcam_link_state_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
#ifdef MXCAM_GESTURE_BENCH_FEED
            bench_t = esp_timer_get_time();
            gesture_bench_feed((uint16_t *)fb->buf); /* test frames instead of the camera, on a schedule */
            bench_feed_us += (uint64_t)(esp_timer_get_time() - bench_t);
#endif
            const uint16_t *frame = (const uint16_t *)fb->buf;
            uint8_t lseq = 0;
            mxp_learn_req_t lreq;
            bool learn;

            mxcam_link_state_lock();
            /* Sample the clock inside the lock, like the link RX task does. */
            ctx.now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            mxs_tick(s_state, ctx.now_ms);
            ctx.connected = mxs_host_connected(s_state, ctx.now_ms);
            ctx.want_frames = mxs_wants_frames(s_state);
            ctx.streaming = s_state->streaming;
            ctx.fps = mxs_effective_fps(s_state);
            ctx.quality = mxs_effective_quality(s_state);
            ctx.fn = s_state->function_id;
            ctx.cfg = s_state->data->cfg;
            ctx.gesture = s_state->data->gesture_cfg;
            copy_colors(&ctx);
            learn = mxs_take_pending_learn(s_state, &lseq, &lreq);
            mxcam_link_state_unlock();
            streaming_now = ctx.streaming;

            /* JPEG work happens on the encoder task (core 0). Here we only
             * copy the clean frame (before the overlay) when a push is due
             * and the encoder is idle; otherwise the frame is simply skipped. */
            if (ctx.want_frames) {
                if (!was_wanting) next_due_ms = ctx.now_ms;
                const uint16_t *enc_src = frame;
#ifdef MXCAM_JPEG_SELFTEST
                if (frame_id % 4 <= 1 && selftest_frame()) enc_src = selftest_frame();
#endif
                if ((int32_t)(ctx.now_ms - next_due_ms) >= 0 &&
                    encoder_try_submit(enc_src, frame_id, ctx.now_ms, ctx.quality)) {
                    uint32_t interval = 1000u / ctx.fps;
                    next_due_ms += interval;
                    if ((int32_t)(ctx.now_ms - next_due_ms) > (int32_t)interval) next_due_ms = ctx.now_ms;
                }
            }
            was_wanting = ctx.want_frames;

            int64_t tp = esp_timer_get_time();
            /* Deferred LEARN, claimed above: sample this frame. Every claimed
             * learn is completed; the response is queued in the state's
             * outbox and sent by the link (core 0), so this core never
             * blocks on USB. A learn cancelled meanwhile changes nothing. */
            if (learn) {
                uint8_t rgb[3];
                bool ok = mxc_sample(frame, lreq.x, lreq.y, lreq.w, lreq.h, rgb);
                mxcam_link_state_lock();
                bool queued = mxs_finish_learn(s_state, lseq, ok ? rgb : NULL);
                copy_colors(&ctx); /* detect the new colour on this very frame */
                mxcam_link_state_unlock();
                if (queued) mxcam_link_kick_deferred();
                if (ok)
                    ESP_LOGI(TAG, "learn seq=%u rgb=%u,%u,%u", lseq, rgb[0], rgb[1], rgb[2]);
                else
                    ESP_LOGI(TAG, "learn seq=%u: no colour", lseq);
            }

            memset(&res, 0, sizeof(res));
            res.frame_id = frame_id;
            res.function_id = ctx.fn;
            bool publish = true; /* false: TFT only (face frame skipped by FACE_EVERY, gesture skip) */
            if (ctx.fn == MXP_FUNC_COLOR) {
                mxc_params_t p = { ctx.cfg.tolerance, ctx.cfg.min_area, ctx.cfg.color_max };
                res.count = mxc_detect(frame, ctx.colors, ctx.n_colors, &p, res.r, MXP_MAX_RESULTS);
            } else if (ctx.fn == MXP_FUNC_APRILTAG) {
#ifdef MXCAM_TAG_BENCH
                bench_frame_start();
#endif
                res.count = mxt_detect(frame, res.r, MXP_MAX_RESULTS, NULL);
            } else if (ctx.fn == MXP_FUNC_FACE) {
                if (face_phase == 0) {
                    uint32_t fus = 0;
                    res.count = mxf_detect(frame, ctx.cfg.face_threshold, ctx.cfg.face_max, res.r, MXP_MAX_RESULTS,
                                           &fus);
                    face_sum_us += fus;
                    if (fus > face_max_us) face_max_us = fus;
                    face_runs++;
                    face_last_n = res.count;
                    memcpy(face_last, res.r, sizeof(face_last));
                } else {
                    res.count = face_last_n;
                    memcpy(res.r, face_last, sizeof(face_last));
                    publish = false;
                }
                face_phase = (face_phase + 1) % FACE_EVERY;
            } else if (ctx.fn == MXP_FUNC_GESTURE) {
                mxh_stats_t gs;
                int n = mxh_detect_tracked(frame, ctx.gesture.hand_threshold, ctx.gesture.gesture_threshold,
                                           ctx.gesture.max_hands, &gest_trk, res.r, MXP_MAX_RESULTS, &gs);
                if (n == MXH_SKIPPED) {
                    /* Heap guard tripped: no new result, not "no hands". */
                    res.count = 0;
                    publish = false;
                    gest_skip++;
                } else {
                    res.count = (uint8_t)n;
                    gest_sum_us += gs.total_us;
                    gest_det_us += gs.detect_us;
                    gest_cls_us += gs.classify_us;
                    if (gs.total_us > gest_max_us) gest_max_us = gs.total_us;
                    gest_runs++;
                    gest_dets += gs.detected;
                    gest_lost += gs.lost;
                }
            }
            if (ctx.fn != MXP_FUNC_FACE) {
                face_phase = 0;
                face_last_n = 0;
            }
            if (ctx.fn != MXP_FUNC_GESTURE) mxh_track_reset(&gest_trk); /* function switch: detect first */
            uint32_t proc_us = (uint32_t)(esp_timer_get_time() - tp);
#ifdef MXCAM_TAG_BENCH
            if (ctx.fn == MXP_FUNC_APRILTAG) bench_frame_end(proc_us);
#endif
            if (ctx.fn == MXP_FUNC_APRILTAG) {
                mxt_profile_t pr;
                mxt_last_profile(&pr);
                tag_sum[0] += pr.gray_us;
                tag_sum[1] += pr.decimate_us + pr.threshold_us;
                tag_sum[2] += pr.unionfind_us + pr.clusters_us;
                tag_sum[3] += pr.fitquads_us;
                tag_sum[4] += pr.decode_us;
                tag_frames++;
            }
            proc_sum_us += proc_us;
            if (proc_us > proc_max_us) proc_max_us = proc_us;
            last_count = res.count;
            last_fn = ctx.fn;
#ifdef MXCAM_FACE_BENCH
            face_bench(frame, &ctx.cfg); /* frame is still clean (no overlay yet) */
#endif
#if defined(MXCAM_GESTURE_BENCH) && !defined(MXCAM_GESTURE_BENCH_FEED)
            gesture_bench_frame(frame); /* frame is still clean (no overlay yet) */
#endif
#ifdef MXCAM_HANDLM_BENCH
            handlm_bench_frame(frame); /* frame is still clean (no overlay yet) */
#endif

            size_t plen = publish ? mxp_pack_results(&res, packed, sizeof(packed)) : 0;
            if (!publish) {
                /* Skipped face frame or gesture heap-guard skip: nothing new
                 * to cache or push; still drop the old boxes if the function
                 * changed meanwhile. */
                mxcam_link_state_lock();
                if (s_state->function_id != ctx.fn) {
                    res.count = 0;
                    face_last_n = 0;
                    face_phase = 0;
                    mxh_track_reset(&gest_trk);
                }
                mxcam_link_state_unlock();
            } else if (plen > 0) {
                /* Cache + push under the lock that SET_FUNCTION takes: a frame
                 * processed with a function that is no longer current is
                 * dropped, and the link's drop of queued RESULTS on a
                 * function change cannot interleave with this push. The push
                 * itself is non-blocking (copy into a slot). */
                mxcam_link_state_lock();
                if (s_state->function_id == ctx.fn) {
                    mxs_set_results(s_state, packed, (uint16_t)plen);
                    if (mxs_wants_results(s_state) && !mxcam_link_push(MXP_CMD_PUSH_RESULTS, packed, (uint16_t)plen))
                        results_push_fail++;
                } else {
                    res.count = 0; /* stale: not cached, pushed or drawn */
                    face_last_n = 0;
                    face_phase = 0;
                    mxh_track_reset(&gest_trk);
                    stale_drop++;
                }
                mxcam_link_state_unlock();
            }
            frame_id++;

            mxo_canvas_t canvas = { (uint16_t *)fb->buf, (int)fb->width, (int)fb->height };
            draw_results(&canvas, &res);

            char text[24];
            snprintf(text, sizeof(text), "%s  %d FPS", fn_name(ctx.fn), fps);
            /* Panel coordinates, mirrored into the frame (the TFT shows the
             * mirror view, ADR-0010): text top-left, icon top-right. */
            mxo_draw_text_mirrored(&canvas, 2, 2, text, MXO_WHITE, MXO_BLACK);
            if (s_boot_error)
                mxo_draw_text_mirrored(&canvas, 2, canvas.height - 10, s_boot_error, MXO_WHITE, MXO_RED);

            /* Connection icon: filled square = host talked to us within 3 s, hollow = not connected. */
            int ix = mxo_mirror_x(&canvas, canvas.width - ICON_SIZE - ICON_MARGIN, ICON_SIZE);
            mxo_fill_rect(&canvas, ix - 2, ICON_MARGIN - 2, ICON_SIZE + 4, ICON_SIZE + 4, MXO_BLACK);
            if (ctx.connected)
                mxo_fill_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, MXO_WHITE);
            else
                mxo_draw_rect(&canvas, ix, ICON_MARGIN, ICON_SIZE, ICON_SIZE, 2, MXO_WHITE);

#ifdef MXCAM_GESTURE_BENCH
            bench_t = esp_timer_get_time();
#endif
            if (mxcam_display_draw((const uint16_t *)fb->buf) != ESP_OK) lcd_err++;
#ifdef MXCAM_GESTURE_BENCH
            bench_lcd_us += (uint64_t)(esp_timer_get_time() - bench_t);
#endif
            mxcam_camera_release(fb);
            status_led_set(res.count > 0 ? STATUS_LED_RESULT : STATUS_LED_IDLE, ctx.cfg.led);
            frames++;
        }

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            fps = (int)((int64_t)frames * 1000000 / (now - t0));
            encoder_stats_t es;
            encoder_take_stats(&es);
            uint32_t q_drop, q_big, tx_fail;
            mxcam_link_take_push_stats(&q_drop, &q_big, &tx_fail);
            unsigned proc_avg_ms = frames ? (unsigned)(proc_sum_us / (uint64_t)frames / 1000u) : 0;
            ESP_LOGI(TAG,
                     "fps=%d fn=%s proc_ms=%u/%u face_ms=%u/%u/%u results=%u stream=%d | enc_ms=%u/%u jpeg=%u sent=%u "
                     "drop=%u reenc=%u big=%u enc_fail=%u | qdrop=%u qbig=%u txfail=%u rfail=%u | "
                     "heap=%u min=%u psram=%u stack_hw=%u enc_stack_hw=%u | grab_fail=%u bad_size=%u lcd_err=%u "
                     "stale=%u",
                     fps, fn_name(last_fn), proc_avg_ms, (unsigned)(proc_max_us / 1000u),
                     face_runs ? (unsigned)(face_sum_us / face_runs / 1000u) : 0u, (unsigned)(face_max_us / 1000u),
                     face_runs, last_count, streaming_now ? 1 : 0, (unsigned)es.enc_avg_ms, (unsigned)es.enc_max_ms,
                     (unsigned)es.jpeg_avg, es.frames, es.send_fail, es.reenc, es.too_big, es.enc_fail,
                     (unsigned)q_drop, (unsigned)q_big, (unsigned)tx_fail, results_push_fail,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)es.stack_hw, grab_fail, bad_size,
                     lcd_err, stale_drop);
            if (tag_frames) {
                /* AprilTag stage averages (ms x10): gray, decimate+threshold,
                 * unionfind+clusters, quad fit, decode+refine. */
                unsigned f = tag_frames;
                ESP_LOGI(TAG, "tag_ms_x10 gray=%u thr=%u seg=%u fit=%u decode=%u",
                         (unsigned)(tag_sum[0] / f / 100), (unsigned)(tag_sum[1] / f / 100),
                         (unsigned)(tag_sum[2] / f / 100), (unsigned)(tag_sum[3] / f / 100),
                         (unsigned)(tag_sum[4] / f / 100));
                memset(tag_sum, 0, sizeof(tag_sum));
                tag_frames = 0;
            }
            if (gest_runs || gest_skip) {
                /* Gesture recognition (ms): total avg/max/runs, detection
                 * average per detection and frames that detected (dets; the
                 * rest reused the boxes), classify average per frame, reuse
                 * frames that lost a hand and detected again, heap-guard
                 * skips this second. */
                unsigned g = gest_runs ? gest_runs : 1, d = gest_dets ? gest_dets : 1;
                ESP_LOGI(TAG, "gest_ms=%u/%u/%u det=%u dets=%u cls=%u lost=%u skip=%u",
                         (unsigned)(gest_sum_us / g / 1000u), (unsigned)(gest_max_us / 1000u), gest_runs,
                         (unsigned)(gest_det_us / d / 1000u), gest_dets, (unsigned)(gest_cls_us / g / 1000u),
                         gest_lost, gest_skip);
                gest_sum_us = gest_det_us = gest_cls_us = 0;
                gest_max_us = 0;
                gest_runs = gest_skip = gest_dets = gest_lost = 0;
            }
#ifdef MXCAM_GESTURE_BENCH
            if (frames)
                ESP_LOGI(TAG, "bench frame_ms=%u grab_ms=%u lcd_ms=%u proc_ms=%u feed_ms=%u",
                         (unsigned)((now - t0) / frames / 1000), (unsigned)(bench_grab_us / (unsigned)frames / 1000u),
                         (unsigned)(bench_lcd_us / (unsigned)frames / 1000u),
                         (unsigned)(proc_sum_us / (unsigned)frames / 1000u),
                         (unsigned)(bench_feed_us / (unsigned)frames / 1000u));
            bench_grab_us = bench_lcd_us = bench_feed_us = 0;
#endif
            frames = 0;
            proc_sum_us = 0;
            proc_max_us = 0;
            face_sum_us = 0;
            face_max_us = 0;
            face_runs = 0;
            t0 = now;
        }
    }
}

esp_err_t pipeline_start(mxs_state_t *state, const char *boot_error)
{
    s_state = state;
    s_boot_error = boot_error;
    if (xTaskCreatePinnedToCore(pipeline_task, "pipeline", PIPE_STACK, NULL, PIPE_PRIO, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
