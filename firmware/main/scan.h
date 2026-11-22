/* QR code (function 6, ADR-0020), shapes (11) and motion (12, ADR-0021) glue
 * for the pipeline task: per-frame copies of their config and the learned QR
 * table, detection, the QR content for GET_RESULT_DATA, LEARN 6 and the TFT
 * overlay. Call every function only from the pipeline task (mxq_detect /
 * mxsh_detect / mxm_detect are not reentrant and the last frame's codes are
 * static). */
#ifndef SCAN_H
#define SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_overlay.h"
#include "mxcam_proto.h"
#include "mxcam_service.h"

/* With the state lock held: when s->data_gen changed since the last call,
 * copies shape_cfg / motion_cfg / the learned QR table (static, PSRAM). */
void scan_sync(const mxs_state_t *s);
/* Function changed (no lock needed): mxm_reset when it is now 12 (warm-up),
 * and the previous frame's QR codes are dropped. */
void scan_on_function(uint8_t fn);
/* No lock: 6 -> mxq_detect + learned id (mxst_qr_lookup) + mxq_to_result,
 * 11 -> mxsh_detect, 12 -> mxm_detect; other functions -> 0. Returns the
 * number of records written to out (<= cap), or -1 when QR detection was
 * skipped (MXQ_SKIPPED: no new result, not "no codes"). */
int scan_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out, uint8_t cap);
/* With the state lock held, right after mxs_set_results: for function 6 hands
 * this frame's code contents to mxs_set_result_data (GET_RESULT_DATA). */
void scan_publish_data(mxs_state_t *s, uint8_t fn);
/* A claimed LEARN 6, after scan_run on this frame: picks the code in the
 * region (mxq_pick_for_learn), takes the state lock, mxs_finish_learn_qr (NULL
 * when no decoded code lies in the region) and re-syncs. The new id applies
 * from the next frame. Returns true when a response was queued (the caller
 * kicks the link). */
bool scan_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req);
/* TFT overlay (plan design decision 16), geometry in frame coordinates, text
 * mirrored (ADR-0010): QR = corner quadrilateral + "QR3 HELLO", shapes = box +
 * "SQUARE 90", motion = white box + "MOVE 63". */
void scan_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res);
/* Once per second (pipeline's stats line): logs
 * "qr_ms=avg/max/runs gray=avg id=avg dec=avg grids=n ok=n fail=n skip=n"
 * when QR ran (or was skipped) this second, then resets the counters. */
void scan_log_stats(const char *tag);

#endif
