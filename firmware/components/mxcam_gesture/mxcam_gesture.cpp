#include "mxcam_gesture.h"

#include <cstring>
#include <list>
#include <new>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "sdkconfig.h"

/* The constructors below silently leave the model unset (or, for the
 * classifier, a dangling pointer) when the model is not selected in
 * menuconfig: make that a build error instead. */
#if !CONFIG_FLASH_ESPDET_PICO_224_224_HAND
#error "mxcam_gesture needs CONFIG_FLASH_ESPDET_PICO_224_224_HAND (see sdkconfig.defaults)"
#endif
#if !CONFIG_FLASH_HAND_GESTURE_CLS_MOBILENETV2_0_5_S8_V1
#error "mxcam_gesture needs CONFIG_FLASH_HAND_GESTURE_CLS_MOBILENETV2_0_5_S8_V1 (see sdkconfig.defaults)"
#endif

static const char *TAG = "mxcam_gesture";

/* esp-dl's own allocations are plain `new` / heap_caps_* without a failure
 * path (C++ exceptions are off, so a failed `new` aborts). mxh_init therefore
 * refuses to start below these free sizes. Measured on the board (M8a Task 1,
 * docs/measurements/m8a.md): loading both models takes 1,827,532 B of PSRAM
 * and no internal RAM; the PSRAM floor is that + 1 MiB (2.74 MiB), rounded
 * up to 3 MiB. Internal floor as mxcam_face. */
#define MXH_MIN_PSRAM_FREE (3u * 1024u * 1024u)
#define MXH_MIN_INTERNAL_FREE (64u * 1024u)

namespace {
/* Expose whether the wrappers actually created their models (load_model only
 * logs when a model is not selected in menuconfig). */
class MxHandDetect : public HandDetect {
public:
    MxHandDetect() : HandDetect(HandDetect::ESPDET_PICO_224_224_HAND, false) {}
    bool loaded() const { return m_model != nullptr; }
};

class MxGestureCls : public HandGestureCls {
public:
    MxGestureCls() : HandGestureCls(HandGestureCls::MOBILENETV2_0_5_S8_V1, false) {}
    bool loaded() const { return m_model != nullptr; }
};
} // namespace

static MxHandDetect *s_det;
static MxGestureCls *s_cls;
static int s_det_thr = -1; /* hand_threshold (0..100) last applied to the detector */

/* Detector score threshold slightly below the user threshold so the rounding
 * in mxh_confidence (pure C, host-tested) is the only filter that decides. */
static void apply_threshold(uint8_t threshold)
{
    if (s_det_thr == threshold) return;
    float thr = ((float)threshold - 0.5f) / 100.0f;
    if (thr < 0.0f) thr = 0.0f;
    s_det->set_score_thr(thr, 0);
    s_det_thr = threshold;
}

/* See MXH_GUARD_* in the header. Warns at most once per second. */
static bool heap_ok(int64_t now_us)
{
    static int64_t s_last_warn_us;
    static bool s_warned;
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (internal >= MXH_GUARD_INTERNAL_FREE && psram_block >= MXH_GUARD_PSRAM_BLOCK) return true;
    if (!s_warned || now_us - s_last_warn_us >= 1000000) {
        s_warned = true;
        s_last_warn_us = now_us;
        ESP_LOGW(TAG, "low memory, recognition skipped (internal free %u, largest PSRAM block %u)",
                 (unsigned)internal, (unsigned)psram_block);
    }
    return false;
}

static bool built(dl::Model *m) { return m && m->is_built(); }

extern "C" bool mxh_init(void)
{
    if (s_det && s_cls) return true;
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (psram < MXH_MIN_PSRAM_FREE || internal < MXH_MIN_INTERNAL_FREE) {
        ESP_LOGE(TAG, "not enough memory (psram %u, internal %u)", (unsigned)psram, (unsigned)internal);
        return false;
    }
#if CONFIG_SPIRAM_USE_MALLOC
    /* As in mxf_init (ADR-0011, ADR-0019): loading builds hundreds of small
     * objects with plain new/malloc that would land in internal RAM; send
     * them to PSRAM while both models load. The switch is global, so call
     * mxh_init at boot before the other tasks start allocating. */
    heap_caps_malloc_extmem_enable(0);
#endif
    MxHandDetect *d = new (std::nothrow) MxHandDetect();
    MxGestureCls *c = new (std::nothrow) MxGestureCls();
#if CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    bool ok = d && c && d->loaded() && c->loaded() && built(d->get_raw_model(0)) && built(c->get_raw_model());
    if (!ok) {
        ESP_LOGE(TAG, "gesture models failed to load");
        delete d;
        delete c;
        return false;
    }
    s_det = d;
    s_cls = c;
    s_det_thr = -1;
    return true;
}

/* Top-1 class of one crop. The box must be clipped to the frame and
 * >= MXH_MIN_HAND_PX on both sides: a valid crop for esp-dl (an empty one
 * asserts). Class names point at esp-dl's static table. */
static void classify(const dl::image::img_t &img, const mxh_raw_box_t &box, const char **name, float *score)
{
    std::vector<int> crop = { (int)box.x0, (int)box.y0, (int)box.x1, (int)box.y1 };
    std::vector<dl::cls::result_t> cls = s_cls->run_crop(img, crop);
    *name = cls.empty() ? nullptr : cls[0].cat_name;
    *score = cls.empty() ? 0.0f : cls[0].score;
}

/* Full detection + classification of the best `limit` hands (as before
 * detection skipping). Updates trk when given. */
static uint8_t detect_and_classify(const dl::image::img_t &img, uint8_t hand_threshold, uint8_t gesture_threshold,
                                   uint8_t max_hands, uint8_t limit, mxh_track_t *trk, mxp_result_t *out,
                                   mxh_stats_t *st)
{
    int64_t t0 = esp_timer_get_time();
    apply_threshold(hand_threshold);
    std::list<dl::detect::result_t> &res = s_det->run(img);
    mxh_raw_box_t raw[MXH_MAX_RAW];
    size_t k = 0;
    for (const dl::detect::result_t &r : res) {
        if (k == MXH_MAX_RAW) break;
        if (r.box.size() < 4) continue;
        raw[k].x0 = r.box[0];
        raw[k].y0 = r.box[1];
        raw[k].x1 = r.box[2];
        raw[k].y1 = r.box[3];
        raw[k].score = r.score;
        k++;
    }
    st->raw_hands = (uint8_t)k;
    /* The detector letterboxes the full frame and maps its boxes back, so
     * they are already true-image 320x240 coordinates. */
    mxh_raw_box_t hands[MXH_MAX_HANDS];
    uint8_t nh = mxh_pick_hands(raw, k, hand_threshold, limit, hands);
    int64_t t1 = esp_timer_get_time();
    st->detect_us += (uint32_t)(t1 - t0);
    st->detected = 1;
    bool any_lost = false;
    for (uint8_t i = 0; i < nh; i++) {
        const char *name;
        float score;
        classify(img, hands[i], &name, &score);
        if (mxh_hand_lost(name)) any_lost = true;
        mxh_to_result(&hands[i], name, score, gesture_threshold, &out[i]);
        st->hand_conf[i] = mxh_confidence(hands[i].score);
    }
    st->classify_us += (uint32_t)(esp_timer_get_time() - t1);
    if (trk) mxh_track_detected(trk, hands, nh, any_lost, hand_threshold, gesture_threshold, max_hands);
    return nh;
}

/* Classifies the tracked boxes (grown, mxh_expand_box) without detection.
 * Returns false as soon as a crop says no_hand (or cannot be built): the
 * caller then detects on this frame; out may hold partial records. */
static bool reuse_boxes(const dl::image::img_t &img, uint8_t gesture_threshold, mxh_track_t *trk,
                        mxp_result_t *out, mxh_stats_t *st)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = true;
    for (uint8_t i = 0; i < trk->n && ok; i++) {
        mxh_raw_box_t crop;
        const char *name = nullptr;
        float score = 0.0f;
        if (mxh_expand_box(&trk->hands[i], &crop)) classify(img, crop, &name, &score);
        if (mxh_hand_lost(name)) {
            ok = false;
        } else {
            /* Reported box and id-0 confidence: the last detection's. */
            mxh_to_result(&trk->hands[i], name, score, gesture_threshold, &out[i]);
            st->hand_conf[i] = mxh_confidence(trk->hands[i].score);
        }
    }
    st->classify_us += (uint32_t)(esp_timer_get_time() - t0);
    return ok;
}

extern "C" int mxh_detect_tracked(const uint16_t *frame, uint8_t hand_threshold, uint8_t gesture_threshold,
                                  uint8_t max_hands, mxh_track_t *trk, mxp_result_t *out, uint8_t cap,
                                  mxh_stats_t *stats)
{
    int64_t t0 = esp_timer_get_time();
    mxh_stats_t st = {};
    int n = MXH_SKIPPED;
    if (s_det && s_cls && frame && out && heap_ok(t0)) {
        n = 0;
        uint8_t limit = max_hands > MXH_MAX_HANDS ? MXH_MAX_HANDS : (max_hands < 1 ? 1 : max_hands);
        if (cap < limit) limit = cap;
        if (limit > MXP_MAX_RESULTS) limit = MXP_MAX_RESULTS;
        if (limit > 0) {
            dl::image::img_t img;
            img.data = const_cast<uint16_t *>(frame); /* read only */
            img.width = MXH_FRAME_W;
            img.height = MXH_FRAME_H;
            /* esp-dl's RGB565BE is the camera's byte order (high byte first). */
            img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
            /* The tracker sees the effective limit (cap included) as max_hands. */
            if (trk && !mxh_track_need_detect(trk, hand_threshold, gesture_threshold, limit, MXH_DETECT_EVERY)) {
                if (reuse_boxes(img, gesture_threshold, trk, out, &st)) {
                    mxh_track_reused(trk);
                    n = trk->n;
                } else {
                    st.lost = 1;
                    memset(st.hand_conf, 0, sizeof(st.hand_conf));
                    n = detect_and_classify(img, hand_threshold, gesture_threshold, limit, limit, trk, out, &st);
                }
            } else {
                n = detect_and_classify(img, hand_threshold, gesture_threshold, limit, limit, trk, out, &st);
            }
        }
    } else if (trk) {
        mxh_track_reset(trk); /* skipped frame: detect on the next one */
    }
    st.total_us = (uint32_t)(esp_timer_get_time() - t0);
    if (stats) *stats = st;
    return n;
}

extern "C" int mxh_detect(const uint16_t *frame, uint8_t hand_threshold, uint8_t gesture_threshold,
                          uint8_t max_hands, mxp_result_t *out, uint8_t cap, mxh_stats_t *stats)
{
    return mxh_detect_tracked(frame, hand_threshold, gesture_threshold, max_hands, nullptr, out, cap, stats);
}

extern "C" void mxh_deinit(void)
{
    delete s_det;
    delete s_cls;
    s_det = nullptr;
    s_cls = nullptr;
    s_det_thr = -1;
}
