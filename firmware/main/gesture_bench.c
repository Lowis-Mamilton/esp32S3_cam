#ifdef MXCAM_GESTURE_BENCH
#include "gesture_bench.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_gesture.h"

/* Used for the M8a Task 1 measurements (docs/measurements/m8a.md). It stalls
 * the TFT for the benchmark runs, so never ship it enabled. app_main loads the
 * models (mxh_init) and logs the load time and memory; this only runs them.
 * With -DMXCAM_GESTURE_BENCH_IMAGE=<file> (whole 320x240 RGB565 big-endian
 * frames, camera byte order) it also runs on each embedded frame, so it can
 * be checked without a hand in front of the camera. */

static const char *TAG = "gesture_bench";

#define GB_W MXH_FRAME_W
#define GB_H MXH_FRAME_H
#define GB_FRAME_BYTES ((size_t)GB_W * GB_H * 2)
#define GB_MAX_IMAGES 8
#define GB_HAND_THR 10
#define GB_GESTURE_THR 0
#define GB_MAX_HANDS 2

#ifdef MXCAM_GESTURE_BENCH_IMAGE
extern const uint8_t gesture_bench_img_start[] asm("_binary_gesture_bench_img_start");
extern const uint8_t gesture_bench_img_end[] asm("_binary_gesture_bench_img_end");
#endif

typedef struct {
    unsigned runs, skipped;
    uint64_t sum_total, sum_detect, sum_classify;
    uint32_t max_total, max_detect, max_classify;
} gb_stats_t;

/* trk NULL: full detection (mxh_detect); otherwise mxh_detect_tracked with
 * that tracker (detection skipping, ADR-0019 addendum). */
static void run_one(const char *label, const uint16_t *frame, uint8_t max_hands, mxh_track_t *trk, gb_stats_t *st)
{
    mxp_result_t r[MXP_MAX_RESULTS];
    mxh_stats_t s;
    heap_caps_monitor_local_minimum_free_size_start();
    int n = mxh_detect_tracked(frame, GB_HAND_THR, GB_GESTURE_THR, max_hands, trk, r, MXP_MAX_RESULTS, &s);
    size_t lmin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    if (n == MXH_SKIPPED) {
        st->skipped++;
        ESP_LOGW(TAG, "%s skipped (%u so far)", label, st->skipped);
        return;
    }
    st->runs++;
    st->sum_total += s.total_us;
    st->sum_detect += s.detect_us;
    st->sum_classify += s.classify_us;
    if (s.total_us > st->max_total) st->max_total = s.total_us;
    if (s.detect_us > st->max_detect) st->max_detect = s.detect_us;
    if (s.classify_us > st->max_classify) st->max_classify = s.classify_us;
    char hands[112] = "";
    int len = 0;
    for (int i = 0; i < n && len < (int)sizeof(hands); i++)
        len += snprintf(hands + len, sizeof(hands) - (size_t)len, " [%u %u %u %d,%d %ux%u]", r[i].id,
                        r[i].confidence, s.hand_conf[i], r[i].x, r[i].y, r[i].w, r[i].h);
    ESP_LOGI(TAG,
             "%s%s total=%u/%u/%u det=%u/%u/%u cls=%u/%u/%u ms (now/avg/max) raw=%u n=%d%s | internal %u (lowest "
             "in call %u) min %u | psram %u block %u | stack_hw %u",
             label, trk ? (s.detected ? (s.lost ? ".L" : ".D") : ".R") : "", (unsigned)(s.total_us / 1000), (unsigned)(st->sum_total / st->runs / 1000),
             (unsigned)(st->max_total / 1000), (unsigned)(s.detect_us / 1000),
             (unsigned)(st->sum_detect / st->runs / 1000), (unsigned)(st->max_detect / 1000),
             (unsigned)(s.classify_us / 1000), (unsigned)(st->sum_classify / st->runs / 1000),
             (unsigned)(st->max_classify / 1000), s.raw_hands, n, hands,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)lmin,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void gesture_bench_frame(const uint16_t *frame)
{
    static bool started;
    static int64_t next_us;
    static gb_stats_t st[1 + GB_MAX_IMAGES];
    /* Tracked runs on the test frames with max_hands 1 and 2 ("imgN/1",
     * "imgN/2"): reuse frames (.R) and detection frames (.D, .L = a reuse
     * crop lost the hand) averaged separately. */
    static gb_stats_t st_reuse[2][GB_MAX_IMAGES], st_det[2][GB_MAX_IMAGES];
    static mxh_track_t trk[2][GB_MAX_IMAGES];
    static uint16_t *imgs; /* test frames copied to PSRAM, like camera buffers */
    static int n_imgs;
    int64_t now = esp_timer_get_time();
    if (!started) {
        started = true;
        ESP_LOGI(TAG, "start: internal %u min %u | psram %u block %u | stack_hw %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
#ifdef MXCAM_GESTURE_BENCH_IMAGE
        size_t bytes = (size_t)(gesture_bench_img_end - gesture_bench_img_start);
        n_imgs = (int)(bytes / GB_FRAME_BYTES);
        if (n_imgs > GB_MAX_IMAGES) n_imgs = GB_MAX_IMAGES;
        if (n_imgs > 0) imgs = heap_caps_malloc(n_imgs * GB_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (imgs)
            memcpy(imgs, gesture_bench_img_start, n_imgs * GB_FRAME_BYTES);
        else
            n_imgs = 0;
        ESP_LOGI(TAG, "%d test frame(s) from %u bytes", n_imgs, (unsigned)bytes);
#endif
        next_us = now + 1000000;
        return;
    }
    if (now < next_us) return;
    next_us = now + 1000000;
    run_one("live", frame, GB_MAX_HANDS, NULL, &st[0]);
    for (int i = 0; i < n_imgs; i++) {
        char label[16];
        const uint16_t *img = imgs + (size_t)i * GB_W * GB_H;
        snprintf(label, sizeof(label), "img%d", i);
        run_one(label, img, GB_MAX_HANDS, NULL, &st[1 + i]);
        /* MXH_DETECT_EVERY tracked frames: one detection + the reuses. */
        for (int m = 0; m < 2; m++) {
            uint8_t mh = (uint8_t)(m + 1);
            snprintf(label, sizeof(label), "img%d/%u", i, mh);
            for (int k = 0; k < MXH_DETECT_EVERY; k++)
                run_one(label, img, mh, &trk[m][i],
                        mxh_track_need_detect(&trk[m][i], GB_HAND_THR, GB_GESTURE_THR, mh, MXH_DETECT_EVERY)
                            ? &st_det[m][i]
                            : &st_reuse[m][i]);
        }
    }
}
#ifdef MXCAM_GESTURE_BENCH_FEED
/* Feed schedule: GB_FEED_SLOT_S seconds per slot, cycling test frame 0,
 * live camera, test frame 1, live, ... */
#define GB_FEED_SLOT_S 10

void gesture_bench_feed(uint16_t *frame)
{
    static const uint8_t *src;
    static int n_imgs = -1, last_slot = -1;
    if (n_imgs < 0) {
        size_t bytes = (size_t)(gesture_bench_img_end - gesture_bench_img_start);
        n_imgs = (int)(bytes / GB_FRAME_BYTES);
        if (n_imgs > GB_MAX_IMAGES) n_imgs = GB_MAX_IMAGES;
        src = gesture_bench_img_start;
        ESP_LOGI(TAG, "feed: %d test frame(s), %d s per slot", n_imgs, GB_FEED_SLOT_S);
    }
    if (n_imgs == 0) return;
    int slot = (int)((esp_timer_get_time() / 1000000 / GB_FEED_SLOT_S) % (2 * n_imgs));
    if (slot != last_slot) {
        last_slot = slot;
        if (slot % 2 == 0)
            ESP_LOGI(TAG, "feed: img%d", slot / 2);
        else
            ESP_LOGI(TAG, "feed: live");
    }
    if (slot % 2 == 0) memcpy(frame, src + (size_t)(slot / 2) * GB_FRAME_BYTES, GB_FRAME_BYTES);
}
#endif
#endif
