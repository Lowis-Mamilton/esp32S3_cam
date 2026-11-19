/* MXCam command service: protocol request -> response, plus stream state.
 * Pure C99, no hardware or RTOS access. NOT thread safe: callers must guard
 * every mxs_* call with one mutex. */
#ifndef MXCAM_SERVICE_H
#define MXCAM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"
#include "mxcam_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MXS_STREAM_TIMEOUT_MS 3000
#define MXS_MAX_FPS 10
/* Default JPEG quality (1-100) when the host sends 0. 60 gives a 320x240
 * frame of about 4.5 KB (measured): ~10 fps stays well under the USB-Serial-JTAG
 * (12 Mbit/s full speed) budget and encodes fast on the otherwise idle core 0. */
#define MXS_DEFAULT_JPEG_QUALITY 60

/* mxs_handle returns this instead of a size when the response is deferred:
 * nothing is sent now.
 * - LEARN (function 1): the pipeline claims it (mxs_take_pending_learn) and
 *   completes it on the next frame (mxs_finish_learn / mxs_complete_learn);
 *   a SET_FUNCTION change or host timeout cancels it (BAD_PARAM). Either way
 *   the response packet is queued in the state's deferred outbox and the link
 *   sends it (mxs_peek_deferred / mxs_ack_deferred) — never the pipeline.
 * - SAVE / FACTORY_RESET: the save worker claims them with
 *   mxs_take_pending_store(), does the NVS work outside the state lock and
 *   answers with mxs_complete_store(). They are never cancelled. */
#define MXS_DEFERRED ((size_t)-1)
#define MXS_DEFERRED_QUEUE 4
#define MXS_DEFERRED_PKT 12 /* LEARN 0xB0 (10 B) or ERROR (11 B) */

/* Largest packed results payload: 6 + 8 x (12 + 18). */
#define MXS_RESULTS_MAX (6 + MXP_MAX_RESULTS * (12 + MXP_RESULT_EXTRA_MAX))
/* Largest response packet mxs_handle produces: GET_LEARNED for AprilTag with
 * 32 names of 48 bytes (1 + 32 x 54 payload bytes). Size `out` with this. */
#define MXS_RESPONSE_MAX (MXP_OVERHEAD + 1 + MXST_MAX_NAMES * (6 + MXP_NAME_MAX))

/* Results the pipeline passes to mxs_complete_learn() (> 0 is the learned id). */
#define MXS_LEARN_NO_COLOR 0 /* region has no usable colour -> BAD_PARAM */
#define MXS_LEARN_FULL (-1)  /* learned list full -> FULL */

typedef struct {
  bool active;  /* a LEARN is outstanding (queued or claimed): further LEARNs get BUSY */
  bool claimed; /* the pipeline took it; it will complete it itself */
  uint8_t seq;
  mxp_learn_req_t req;
} mxs_pending_learn_t;

typedef struct {
  uint8_t seq, len;
  uint8_t pkt[MXS_DEFERRED_PKT];
} mxs_deferred_pkt_t;

typedef struct {
  bool active;  /* a SAVE / FACTORY_RESET is outstanding: further ones get BUSY */
  bool claimed; /* the save worker took it */
  uint8_t cmd;  /* MXP_CMD_SAVE or MXP_CMD_FACTORY_RESET */
  uint8_t seq;
  uint32_t gen; /* data_gen when the save worker took its snapshot */
} mxs_pending_store_t;

typedef struct {
  mxp_info_t info;
  /* Config + learned data + names (owned by the caller; loaded from NVS at
   * boot). data->cfg.dirty = changed since the last load/save. */
  mxs_data_t *data;
  uint32_t data_gen; /* bumped by every change of *data */
  uint8_t results[MXS_RESULTS_MAX]; /* GET_RESULTS: latest processed frame, packed */
  uint16_t results_len;             /* 0 = no frame processed yet */
  mxs_pending_store_t pending_store;
  /* Scratch for building large response payloads (GET_LEARNED). */
  uint8_t scratch[MXS_RESPONSE_MAX];
  mxp_learned_t learned_scratch[MXST_MAX_NAMES];
  uint8_t function_id;
  bool streaming;
  mxp_stream_start_t stream;
  uint32_t last_request_ms;
  bool has_request; /* at least one valid request received */
  mxs_pending_learn_t pending_learn;
  mxs_deferred_pkt_t outbox[MXS_DEFERRED_QUEUE]; /* deferred responses not sent yet, oldest first */
  uint8_t outbox_count;
  bool results_invalidated; /* function changed: drop queued RESULTS pushes */
} mxs_state_t;

/* `data` must outlive the state; its dirty flag is cleared (freshly loaded). */
void mxs_init(mxs_state_t *s, const mxp_info_t *info, mxs_data_t *data);

/* Handle one request, write the response packet to out and return its size
 * (0 if out_cap is too small; MXS_RESPONSE_MAX always suffices).
 * Function IDs: SET_FUNCTION accepts 0 (none), 1 (colour), 2 (AprilTag).
 * Every request is treated as coming over USB, so STREAM_START/STOP are
 * always allowed (the NOT_SUPPORTED path for UART/I2C arrives with the
 * transport flag in M4). */
size_t mxs_handle(mxs_state_t *s, const mxp_frame_t *req, uint32_t now_ms, uint8_t *out,
                  size_t out_cap);

/* Pipeline claims the queued LEARN (once). The learn stays active (BUSY for new
 * ones) until it is completed or cancelled. */
bool mxs_take_pending_learn(mxs_state_t *s, uint8_t *seq, mxp_learn_req_t *req);
/* Queues the LEARN response for the claimed learn `seq` (0xB0 with the id for
 * result > 0, ERROR BAD_PARAM for MXS_LEARN_NO_COLOR, FULL for MXS_LEARN_FULL)
 * and clears it. false (nothing queued) if no learn with that seq is claimed,
 * e.g. it was cancelled meanwhile. */
bool mxs_complete_learn(mxs_state_t *s, uint8_t seq, int result);

/* Deferred response delivery (link): peek the front packet (0 if none or
 * out_cap is too small; seq in *seq), send it, and only after a successful
 * send call mxs_ack_deferred(); a failed send is retried at the next flush.
 * The outbox holds MXS_DEFERRED_QUEUE packets, the oldest is dropped on
 * overflow. */
size_t mxs_peek_deferred(const mxs_state_t *s, uint8_t *seq, uint8_t *out, size_t out_cap);
void mxs_ack_deferred(mxs_state_t *s);

/* Functions that actually work (set at boot from what initialised; GET_INFO
 * lists them, SET_FUNCTION accepts only them and 0). If the current function
 * is no longer available it falls back to 0. */
void mxs_set_capabilities(mxs_state_t *s, const uint8_t *ids, uint8_t n);

/* true once after a function change: queued RESULTS pushes are stale. The
 * link calls it under the state lock after every request. */
bool mxs_take_results_invalidated(mxs_state_t *s);

/* Pipeline-side LEARN completion: if `seq` is still the claimed learn, learns
 * `rgb` (NULL = the region had no usable colour) into data via
 * mxst_learn_color with the current tolerance (dirty only if the colours
 * changed) and queues the response like mxs_complete_learn. false and no
 * change when the learn was cancelled meanwhile. */
bool mxs_finish_learn(mxs_state_t *s, uint8_t seq, const uint8_t *rgb);

/* Save worker: claims the pending SAVE / FACTORY_RESET (once). For SAVE,
 * *snapshot receives a copy of the data to write (may be NULL for a reset). */
bool mxs_take_pending_store(mxs_state_t *s, uint8_t *cmd, uint8_t *seq, mxs_data_t *snapshot);
/* Completes the claimed store op `seq`: ok -> empty 0xC2 / 0xC3 (SAVE clears
 * dirty unless data changed after the snapshot; FACTORY_RESET loads the
 * defaults into data), !ok -> ERROR INTERNAL with data unchanged. Returns the
 * packet size, or 0 when nothing with that seq is claimed. */
size_t mxs_complete_store(mxs_state_t *s, uint8_t seq, bool ok, uint8_t *out, size_t out_cap);

/* Pipeline: stores the packed results of the latest processed frame for
 * GET_RESULTS (ignored when len > MXS_RESULTS_MAX). The caller must drop a
 * frame processed with another function than s->function_id (checked under
 * the same lock). A function change resets the cache to "no records" for the
 * new function with the last frame_id. */
void mxs_set_results(mxs_state_t *s, const uint8_t *packed, uint16_t len);

/* Stops streaming when no request arrived for more than 3000 ms (and cancels a
 * pending LEARN). */
void mxs_tick(mxs_state_t *s, uint32_t now_ms);
/* A request arrived within the last 3000 ms. */
bool mxs_host_connected(const mxs_state_t *s, uint32_t now_ms);
/* Streaming and the host asked for FRAME pushes (flags bit0). */
bool mxs_wants_frames(const mxs_state_t *s);
/* Streaming and the host asked for RESULTS pushes (flags bit1). */
bool mxs_wants_results(const mxs_state_t *s);
uint8_t mxs_effective_fps(const mxs_state_t *s);     /* 0 -> 10, capped at 10 */
uint8_t mxs_effective_quality(const mxs_state_t *s); /* 0 -> default, capped at 100 */

#ifdef __cplusplus
}
#endif

#endif
