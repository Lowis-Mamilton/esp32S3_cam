/* MXCam command service: protocol request -> response, plus stream state.
 * Pure C99, no hardware or RTOS access. NOT thread safe: callers must guard
 * every mxs_* call with one mutex. */
#ifndef MXCAM_SERVICE_H
#define MXCAM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MXS_STREAM_TIMEOUT_MS 3000
#define MXS_MAX_FPS 10
/* Default JPEG quality (1-100) when the host sends 0. 60 gives a 320x240
 * frame of about 4.5 KB (measured): ~10 fps stays well under the USB-Serial-JTAG
 * (12 Mbit/s full speed) budget and encodes fast on the otherwise idle core 0. */
#define MXS_DEFAULT_JPEG_QUALITY 60

/* mxs_handle returns this instead of a size when the response is deferred
 * (LEARN on function 1): nothing is sent now; the pipeline later completes the
 * request via mxs_take_pending_learn() + mxs_complete_learn(), or it is
 * cancelled (mxs_take_cancelled()). */
#define MXS_DEFERRED ((size_t)-1)

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
  mxp_info_t info;
  uint8_t function_id;
  bool streaming;
  mxp_stream_start_t stream;
  uint32_t last_request_ms;
  bool has_request; /* at least one valid request received */
  mxs_pending_learn_t pending_learn;
  bool cancel_pending; /* an unclaimed learn was cancelled; its error is not sent yet */
  uint8_t cancel_seq;
} mxs_state_t;

void mxs_init(mxs_state_t *s, const mxp_info_t *info);

/* Handle one request, write the response packet to out and return its size
 * (0 if out_cap is too small). M1: every request is treated as coming over
 * USB, so STREAM_START/STOP are always allowed (the NOT_SUPPORTED path for
 * UART/I2C arrives with the transport flag in M4). */
size_t mxs_handle(mxs_state_t *s, const mxp_frame_t *req, uint32_t now_ms, uint8_t *out,
                  size_t out_cap);

/* Pipeline claims the queued LEARN (once). The learn stays active (BUSY for new
 * ones) until mxs_complete_learn(). */
bool mxs_take_pending_learn(mxs_state_t *s, uint8_t *seq, mxp_learn_req_t *req);
/* Builds the LEARN response for `seq` (0xB0 with the id, or ERROR BAD_PARAM /
 * FULL) and clears the pending learn. Returns 0 if no learn with that seq is
 * pending or out is too small. */
size_t mxs_complete_learn(mxs_state_t *s, uint8_t seq, int result, uint8_t *out, size_t out_cap);
/* A queued (unclaimed) LEARN is cancelled by SET_FUNCTION change or host/stream
 * timeout. Returns the ERROR BAD_PARAM packet (original seq in *seq) once, 0 if
 * nothing was cancelled. The link calls it after every handle and on its idle
 * wakeups so the cancellation actually goes out. */
size_t mxs_take_cancelled(mxs_state_t *s, uint8_t *seq, uint8_t *out, size_t out_cap);

/* Stops streaming when no request arrived for more than 3000 ms (and cancels an
 * unclaimed LEARN). */
void mxs_tick(mxs_state_t *s, uint32_t now_ms);
/* A request arrived within the last 3000 ms. */
bool mxs_host_connected(const mxs_state_t *s, uint32_t now_ms);
/* Streaming and the host asked for FRAME pushes (flags bit0). */
bool mxs_wants_frames(const mxs_state_t *s);
uint8_t mxs_effective_fps(const mxs_state_t *s);     /* 0 -> 10, capped at 10 */
uint8_t mxs_effective_quality(const mxs_state_t *s); /* 0 -> default, capped at 100 */

#ifdef __cplusplus
}
#endif

#endif
