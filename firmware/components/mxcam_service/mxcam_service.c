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

/* Cancel a queued (not yet claimed) LEARN; its BAD_PARAM goes out via mxs_take_cancelled. */
static void cancel_learn(mxs_state_t *s)
{
  if (s->pending_learn.active && !s->pending_learn.claimed) {
    s->cancel_pending = true;
    s->cancel_seq = s->pending_learn.seq;
    s->pending_learn.active = false;
  }
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
    if (s->function_id != req->payload[0]) cancel_learn(s);
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
  case MXP_CMD_LEARN: {
    mxp_learn_req_t lr;
    if (!mxp_unpack_learn(req->payload, req->len, &lr))
      return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    if (lr.function_id != MXP_FUNC_COLOR || s->function_id != MXP_FUNC_COLOR)
      return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
    if (s->pending_learn.active) return reply_err(req, MXP_ERR_BUSY, out, out_cap);
    s->pending_learn.active = true;
    s->pending_learn.claimed = false;
    s->pending_learn.seq = req->seq;
    s->pending_learn.req = lr;
    return MXS_DEFERRED;
  }
  default:
    return reply_err(req, MXP_ERR_UNKNOWN_CMD, out, out_cap);
  }
}

/* Callers sample the clock on different tasks, so a request may be stamped
 * slightly after "now": treat a negative difference as 0 elapsed. */
static uint32_t elapsed_ms(const mxs_state_t *s, uint32_t now_ms)
{
  int32_t d = (int32_t)(now_ms - s->last_request_ms);
  return d < 0 ? 0 : (uint32_t)d;
}

bool mxs_take_pending_learn(mxs_state_t *s, uint8_t *seq, mxp_learn_req_t *req)
{
  if (!s->pending_learn.active || s->pending_learn.claimed) return false;
  s->pending_learn.claimed = true;
  *seq = s->pending_learn.seq;
  *req = s->pending_learn.req;
  return true;
}

size_t mxs_complete_learn(mxs_state_t *s, uint8_t seq, int result, uint8_t *out, size_t out_cap)
{
  if (!s->pending_learn.active || s->pending_learn.seq != seq) return 0;
  size_t n;
  if (result > 0 && result <= 255) {
    uint8_t id = (uint8_t)result;
    n = mxp_encode(MXP_RESPONSE(MXP_CMD_LEARN), seq, &id, 1, out, out_cap);
  } else {
    uint8_t code = result == MXS_LEARN_FULL ? MXP_ERR_FULL : MXP_ERR_BAD_PARAM;
    n = mxp_encode_error(seq, MXP_CMD_LEARN, code, out, out_cap);
  }
  if (n > 0) s->pending_learn.active = false;
  return n;
}

size_t mxs_take_cancelled(mxs_state_t *s, uint8_t *seq, uint8_t *out, size_t out_cap)
{
  if (!s->cancel_pending) return 0;
  size_t n = mxp_encode_error(s->cancel_seq, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM, out, out_cap);
  if (n == 0) return 0;
  *seq = s->cancel_seq;
  s->cancel_pending = false;
  return n;
}

void mxs_tick(mxs_state_t *s, uint32_t now_ms)
{
  if (elapsed_ms(s, now_ms) > MXS_STREAM_TIMEOUT_MS) {
    s->streaming = false;
    cancel_learn(s);
    /* Forget the timestamp so it cannot wrap around into the "future" (>24.8
     * days of uptime) and read as a recent request. */
    s->has_request = false;
  }
}

bool mxs_host_connected(const mxs_state_t *s, uint32_t now_ms)
{
  return s->has_request && elapsed_ms(s, now_ms) <= MXS_STREAM_TIMEOUT_MS;
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
