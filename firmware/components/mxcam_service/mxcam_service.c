#include "mxcam_service.h"

#include <string.h>

void mxs_init(mxs_state_t *s, const mxp_info_t *info)
{
  memset(s, 0, sizeof(*s));
  s->info = *info;
}

static size_t reply(const mxp_frame_t *req, const uint8_t *payload, uint16_t len, uint8_t *out,
                    size_t out_cap)
{
  return mxp_encode(MXP_RESPONSE(req->cmd), req->seq, payload, len, out, out_cap);
}

static size_t reply_err(const mxp_frame_t *req, uint8_t code, uint8_t *out, size_t out_cap)
{
  return mxp_encode_error(req->seq, req->cmd, code, out, out_cap);
}

size_t mxs_handle(mxs_state_t *s, const mxp_frame_t *req, uint32_t now_ms, uint8_t *out,
                  size_t out_cap)
{
  /* Any valid request counts as a keep-alive. */
  s->last_request_ms = now_ms;
  s->has_request = true;

  switch (req->cmd) {
  case MXP_CMD_GET_INFO: {
    if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    uint8_t buf[23 + 8];
    size_t n = mxp_pack_info(&s->info, buf, sizeof(buf));
    if (n == 0) return reply_err(req, MXP_ERR_INTERNAL, out, out_cap);
    return reply(req, buf, (uint16_t)n, out, out_cap);
  }
  case MXP_CMD_PING:
    if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    return reply(req, NULL, 0, out, out_cap);
  case MXP_CMD_SET_FUNCTION: {
    if (req->len != 1) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    /* M1 supports only function 0 (none); the info lists no others. */
    if (req->payload[0] != MXP_FUNC_NONE) return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
    s->function_id = req->payload[0];
    return reply(req, &s->function_id, 1, out, out_cap);
  }
  case MXP_CMD_GET_FUNCTION:
    if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    return reply(req, &s->function_id, 1, out, out_cap);
  case MXP_CMD_STREAM_START: {
    mxp_stream_start_t p;
    if (!mxp_unpack_stream_start(req->payload, req->len, &p))
      return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    s->stream = p;
    s->streaming = true;
    return reply(req, NULL, 0, out, out_cap);
  }
  case MXP_CMD_STREAM_STOP:
    if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    s->streaming = false;
    return reply(req, NULL, 0, out, out_cap);
  default:
    return reply_err(req, MXP_ERR_UNKNOWN_CMD, out, out_cap);
  }
}

void mxs_tick(mxs_state_t *s, uint32_t now_ms)
{
  if (s->streaming && (uint32_t)(now_ms - s->last_request_ms) > MXS_STREAM_TIMEOUT_MS)
    s->streaming = false;
}

bool mxs_host_connected(const mxs_state_t *s, uint32_t now_ms)
{
  return s->has_request && (uint32_t)(now_ms - s->last_request_ms) <= MXS_STREAM_TIMEOUT_MS;
}

bool mxs_wants_frames(const mxs_state_t *s)
{
  return s->streaming && (s->stream.flags & 1);
}

uint8_t mxs_effective_fps(const mxs_state_t *s)
{
  uint8_t f = s->stream.max_fps;
  return (f == 0 || f > MXS_MAX_FPS) ? MXS_MAX_FPS : f;
}

uint8_t mxs_effective_quality(const mxs_state_t *s)
{
  uint8_t q = s->stream.jpeg_quality;
  if (q == 0) return MXS_DEFAULT_JPEG_QUALITY;
  return q > 100 ? 100 : q;
}
