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
/* Default JPEG quality (1-100) when the host sends 0. 60 keeps a 320x240
 * frame around 8-12 KB: ~10 fps stays well under the USB-Serial-JTAG
 * (12 Mbit/s full speed) budget and encodes fast on the otherwise idle core 0. */
#define MXS_DEFAULT_JPEG_QUALITY 60

typedef struct {
  mxp_info_t info;
  uint8_t function_id;
  bool streaming;
  mxp_stream_start_t stream;
  uint32_t last_request_ms;
  bool has_request; /* at least one valid request received */
} mxs_state_t;

void mxs_init(mxs_state_t *s, const mxp_info_t *info);

/* Handle one request, write the response packet to out and return its size
 * (0 if out_cap is too small). M1: every request is treated as coming over
 * USB, so STREAM_START/STOP are always allowed (the NOT_SUPPORTED path for
 * UART/I2C arrives with the transport flag in M4). */
size_t mxs_handle(mxs_state_t *s, const mxp_frame_t *req, uint32_t now_ms, uint8_t *out,
                  size_t out_cap);

/* Stops streaming when no request arrived for more than 3000 ms. */
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
