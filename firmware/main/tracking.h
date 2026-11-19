/* Line tracking (function 4) and object tracking (function 5) glue for the
 * pipeline task (ADR-0018): per-frame copies of their config and learned
 * data, LEARN, detection and the TFT overlay. Call every function only from
 * the pipeline task (mxl_detect / mxk_learn / mxk_track are not reentrant and
 * the tracker state is static). */
#ifndef TRACKING_H
#define TRACKING_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_overlay.h"
#include "mxcam_proto.h"
#include "mxcam_service.h"

/* With the state lock held: when s->data_gen changed since the last call,
 * copies line_cfg / track_cfg / the line colour / the object; a changed (or
 * removed) object re-arms the tracker (mxk_reset). */
void tracking_sync(const mxs_state_t *s);
/* Function changed (no lock needed): clears the line state and re-arms the
 * object tracker. */
void tracking_on_function(uint8_t fn);
/* No lock: 4 -> mxl_detect, 5 -> mxk_track (0 when no object is learned);
 * other functions -> 0. Writes at most one record to out. */
uint8_t tracking_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out);
/* A claimed LEARN for function 4 (mxc_sample -> mxs_finish_learn) or 5
 * (mxk_learn -> mxs_finish_learn_object). Takes the state lock itself and
 * re-syncs under it, so the new colour / object applies to this very frame.
 * Returns true when a response was queued (the caller kicks the link). */
bool tracking_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req, const uint16_t *frame);
/* TFT overlay (plan design decision 14), after the generic boxes: line = ROI
 * top edge, picked band centres, fitted segment and an "A+8 O-20 X Y" label;
 * object = box with "OBJ 87". Geometry in frame coordinates, text mirrored
 * (ADR-0010). */
void tracking_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res);
/* Top-left function name: "LINE", "TRACK", "TRACK LOST" (learned, not found
 * on the last frame) or "TRACK -" (nothing learned). */
const char *tracking_status(uint8_t fn);

#endif
