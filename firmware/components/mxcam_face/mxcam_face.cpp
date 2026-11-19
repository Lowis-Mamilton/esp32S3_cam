#include "mxcam_face.h"

#include <list>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"
#include "sdkconfig.h"
#include "mxcam_face_post.h"

static const char *TAG = "mxcam_face";

/* esp-dl's own allocations are plain `new` / heap_caps_* without a failure
 * path (C++ exceptions are off, so a failed `new` aborts). mxf_init therefore
 * refuses to start below these free sizes. Measured on the board (M3 Task 1):
 * loading both models takes ~425 KB of PSRAM and < 1 KB of internal RAM (see
 * the extmem switch below); each detection then adds < 1 KB internal and
 * ~10 KB PSRAM once. The internal floor keeps the plan's 60 KB minimum. */
#define MXF_MIN_PSRAM_FREE (1024u * 1024u)
#define MXF_MIN_INTERNAL_FREE (64u * 1024u)

/* Upper bound on the boxes taken from the detector per frame (stage 2 keeps
 * at most 10 after NMS). */
#define MXF_MAX_RAW 16

namespace {
/* Exposes whether HumanFaceDetect actually created its model (load_model
 * only logs when the model is not selected in menuconfig). */
class MxFaceDetect : public HumanFaceDetect {
public:
    MxFaceDetect() : HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false) {}
    bool loaded() const { return m_model != nullptr; }
};
} // namespace

static MxFaceDetect *s_det;
static int s_stage2_thr = -1; /* threshold (0..100) last applied to stage 2 */

/* Stage 2 (MNP) score threshold, slightly below the user threshold so the
 * rounding in mxf_confidence (pure C, host-tested) is the only filter that
 * decides. Stage 1 (MSR) keeps its default 0.5: it only proposes candidates,
 * and lowering it multiplies the stage-2 runs (~7 ms each). */
static void apply_threshold(uint8_t threshold)
{
    if (s_stage2_thr == threshold) return;
    float thr = ((float)threshold - 0.5f) / 100.0f;
    if (thr < 0.0f) thr = 0.0f;
    s_det->set_score_thr(thr, 1);
    s_stage2_thr = threshold;
}

extern "C" bool mxf_init(void)
{
    if (s_det) return true;
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (psram < MXF_MIN_PSRAM_FREE || internal < MXF_MIN_INTERNAL_FREE) {
        ESP_LOGE(TAG, "not enough memory (psram %u, internal %u)", (unsigned)psram, (unsigned)internal);
        return false;
    }
#if CONFIG_SPIRAM_USE_MALLOC
    /* Loading builds hundreds of small objects (modules, tensor descriptors,
     * std containers) with plain new/malloc, which would land in internal RAM
     * (<= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL): ~15 KB measured. Send them to
     * PSRAM while the models load (inference speed is unchanged). The switch
     * is global for ~80 ms, so call mxf_init at boot before the other tasks
     * start allocating. */
    heap_caps_malloc_extmem_enable(0);
#endif
    MxFaceDetect *d = new (std::nothrow) MxFaceDetect();
#if CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    if (!d) return false;
    bool ok = d->loaded();
    if (ok) {
        dl::Model *m0 = d->get_raw_model(0);
        dl::Model *m1 = d->get_raw_model(1);
        ok = m0 && m1 && m0->is_built() && m1->is_built();
    }
    if (!ok) {
        ESP_LOGE(TAG, "face model failed to load");
        delete d;
        return false;
    }
    s_det = d;
    s_stage2_thr = -1;
    return true;
}

extern "C" uint8_t mxf_detect(const uint16_t *frame, uint8_t threshold, uint8_t max_results, mxp_result_t *out,
                              uint8_t cap, uint32_t *elapsed_us)
{
    int64_t t0 = esp_timer_get_time();
    uint8_t n = 0;
    if (s_det && frame && out) {
        apply_threshold(threshold);
        dl::image::img_t img;
        img.data = const_cast<uint16_t *>(frame); /* read only */
        img.width = MXF_FRAME_W;
        img.height = MXF_FRAME_H;
        /* esp-dl's RGB565BE is the camera's byte order: the high byte
         * (RRRRRGGG) first in memory. */
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
        std::list<dl::detect::result_t> &res = s_det->run(img);
        mxf_raw_box_t raw[MXF_MAX_RAW];
        size_t k = 0;
        for (const dl::detect::result_t &r : res) {
            if (k == MXF_MAX_RAW) break;
            if (r.box.size() < 4) continue;
            raw[k].x0 = r.box[0];
            raw[k].y0 = r.box[1];
            raw[k].x1 = r.box[2];
            raw[k].y1 = r.box[3];
            raw[k].score = r.score;
            k++;
        }
        /* The detector worked on the full frame, so its boxes are already
         * in true-image coordinates (scale 1). */
        n = mxf_select(raw, k, MXF_FRAME_W, MXF_FRAME_H, threshold, max_results, out, cap);
    }
    if (elapsed_us) *elapsed_us = (uint32_t)(esp_timer_get_time() - t0);
    return n;
}

extern "C" void mxf_deinit(void)
{
    delete s_det;
    s_det = nullptr;
    s_stage2_thr = -1;
}
