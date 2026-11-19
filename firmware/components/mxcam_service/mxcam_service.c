#include "mxcam_service.h"

#include <string.h>

void mxs_init(mxs_state_t *s, const mxp_info_t *info, mxs_data_t *data)
{
  memset(s, 0, sizeof(*s));
  s->info = *info;
  s->data = data;
  data->cfg.dirty = 0;
}

static void mark_dirty(mxs_state_t *s)
{
  s->data->cfg.dirty = 1;
  s->data_gen++;
}

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static size_t reply(const mxp_frame_t *req, const uint8_t *payload, uint16_t len, uint8_t *out,
                    size_t out_cap)
{
  return mxp_encode(MXP_RESPONSE(req->cmd), req->seq, payload, len, out, out_cap);
}

static size_t reply_err(const mxp_frame_t *req, uint8_t code, uint8_t *out, size_t out_cap)
{
  return mxp_encode_error(req->seq, req->cmd, code, out, out_cap);
}

static bool color_learned(const mxs_data_t *d, uint16_t id)
{
  for (int i = 0; i < d->n_colors; i++)
    if (d->colors[i].id == id) return true;
  return false;
}

/* FORGET: function_id u8, id u16 (0xFFFF = all). Absent ids succeed; face
 * detection (3) learns and names nothing, so it always succeeds. */
static size_t handle_forget(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len != 3) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  uint8_t fn = req->payload[0];
  uint16_t id = get_u16(req->payload + 1);
  uint8_t nc = s->data->n_colors, nn = s->data->n_names; /* forgetting only ever removes */
  if (fn == MXP_FUNC_COLOR) {
    if (id == 0xFFFF) mxst_forget_color(s->data, 0xFF);
    else if (id >= 1 && id <= MXST_MAX_COLORS) mxst_forget_color(s->data, (uint8_t)id);
  } else if (fn == MXP_FUNC_APRILTAG) {
    mxst_forget_names(s->data, MXST_FN_TAG, id); /* tags are not learned: only names */
  } else if (fn == MXP_FUNC_FACE) {
    /* nothing to forget */
  } else {
    return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  }
  if (s->data->n_colors != nc || s->data->n_names != nn) mark_dirty(s);
  return reply(req, NULL, 0, out, out_cap);
}

/* SET_NAME: lengths are checked first (BAD_LENGTH), then UTF-8 and ids
 * (BAD_PARAM); a full name table answers FULL. */
static size_t handle_set_name(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len < 4 || req->payload[3] > MXP_NAME_MAX || req->len != 4u + req->payload[3])
    return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  mxp_set_name_t sn;
  if (!mxp_unpack_set_name(req->payload, req->len, &sn)) /* only invalid UTF-8 is left */
    return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  if (sn.function_id == MXP_FUNC_COLOR) {
    if (!color_learned(s->data, sn.id)) return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  } else if (sn.function_id == MXP_FUNC_APRILTAG) {
    if (sn.id > MXST_TAG_ID_MAX) return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  } else {
    return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  }
  uint8_t old_len = 0;
  const char *old = mxst_get_name(s->data, sn.function_id, sn.id, &old_len);
  bool same = old ? (old_len == sn.name_len && memcmp(old, sn.name, sn.name_len) == 0) : sn.name_len == 0;
  if (same) return reply(req, NULL, 0, out, out_cap);
  /* key and UTF-8 were checked above, so false means the name table is full */
  if (!mxst_set_name(s->data, sn.function_id, sn.id, sn.name, sn.name_len))
    return reply_err(req, MXP_ERR_FULL, out, out_cap);
  mark_dirty(s);
  return reply(req, NULL, 0, out, out_cap);
}

/* GET_LEARNED: colours (by id, with rgb and name) or named tags (by id, rgb 0). */
static size_t handle_get_learned(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len != 1) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  uint8_t fn = req->payload[0];
  const mxs_data_t *d = s->data;
  mxp_learned_t *items = s->learned_scratch;
  uint8_t n = 0;
  if (fn == MXP_FUNC_COLOR) {
    for (int i = 0; i < d->n_colors; i++) {
      mxp_learned_t *it = &items[n++];
      memset(it, 0, sizeof(*it));
      it->id = d->colors[i].id;
      memcpy(it->rgb, d->colors[i].rgb, 3);
      const char *nm = mxst_get_name(d, MXST_FN_COLOR, it->id, &it->name_len);
      if (nm) memcpy(it->name, nm, it->name_len);
      else it->name_len = 0;
    }
  } else if (fn == MXP_FUNC_APRILTAG) {
    for (int i = 0; i < d->n_names; i++) {
      if (d->names[i].fn != MXST_FN_TAG) continue;
      mxp_learned_t *it = &items[n++];
      memset(it, 0, sizeof(*it));
      it->id = d->names[i].id;
      it->name_len = d->names[i].len;
      memcpy(it->name, d->names[i].name, d->names[i].len);
    }
  } else {
    return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  }
  /* sort by id (insertion sort, n <= 32) */
  for (int i = 1; i < n; i++) {
    mxp_learned_t t = items[i];
    int j = i - 1;
    while (j >= 0 && items[j].id > t.id) {
      items[j + 1] = items[j];
      j--;
    }
    items[j + 1] = t;
  }
  size_t len = mxp_pack_learned(items, n, s->scratch, sizeof(s->scratch));
  if (len == 0) return reply_err(req, MXP_ERR_INTERNAL, out, out_cap);
  return reply(req, s->scratch, (uint16_t)len, out, out_cap);
}

/* GET_RESULTS: the latest processed frame; frame 0 / no records before any. */
static size_t handle_get_results(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  if (s->results_len > 0) return reply(req, s->results, s->results_len, out, out_cap);
  mxp_results_t none;
  memset(&none, 0, sizeof(none));
  none.function_id = s->function_id;
  uint8_t buf[8];
  size_t n = mxp_pack_results(&none, buf, sizeof(buf));
  return reply(req, buf, (uint16_t)n, out, out_cap);
}

/* SAVE / FACTORY_RESET: deferred to the save worker (one at a time). */
static size_t handle_store_op(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  if (s->pending_store.active) return reply_err(req, MXP_ERR_BUSY, out, out_cap);
  memset(&s->pending_store, 0, sizeof(s->pending_store));
  s->pending_store.active = true;
  s->pending_store.cmd = req->cmd;
  s->pending_store.seq = req->seq;
  return MXS_DEFERRED;
}

static size_t handle_set_config(mxs_state_t *s, const mxp_frame_t *req, uint8_t *out, size_t out_cap)
{
  if (req->len != MXP_CONFIG_SIZE) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
  mxp_config_t cfg;
  if (!mxp_unpack_config(req->payload, req->len, &cfg)) /* config_version != 1 */
    return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
  /* Compare the packed forms (no struct padding), dirty excluded. */
  uint8_t a[MXP_CONFIG_SIZE], b[MXP_CONFIG_SIZE];
  mxp_config_t cur = s->data->cfg;
  cur.dirty = 0;
  mxp_pack_config(&cur, a);
  mxp_pack_config(&cfg, b); /* cfg.dirty is 0 from unpack */
  if (memcmp(a, b, sizeof(a)) != 0) {
    cfg.dirty = s->data->cfg.dirty;
    s->data->cfg = cfg; /* clamped / fallen back by the codec */
    mark_dirty(s);
  }
  uint8_t c[MXP_CONFIG_SIZE];
  mxp_pack_config(&s->data->cfg, c);
  return reply(req, c, MXP_CONFIG_SIZE, out, out_cap);
}

/* Queue a deferred response packet (sent by the link's flush). Full: the
 * oldest is dropped (the host times out on it). */
static void outbox_push_packet(mxs_state_t *s, uint8_t seq, uint8_t cmd, const uint8_t *payload,
                               uint16_t len, bool is_error, uint8_t code)
{
  if (s->outbox_count == MXS_DEFERRED_QUEUE) {
    memmove(&s->outbox[0], &s->outbox[1], sizeof(s->outbox[0]) * (MXS_DEFERRED_QUEUE - 1));
    s->outbox_count--;
  }
  mxs_deferred_pkt_t *o = &s->outbox[s->outbox_count];
  size_t n = is_error ? mxp_encode_error(seq, cmd, code, o->pkt, sizeof(o->pkt))
                      : mxp_encode(MXP_RESPONSE(cmd), seq, payload, len, o->pkt, sizeof(o->pkt));
  if (n == 0) return; /* cannot happen: packets are at most 11 bytes */
  o->len = (uint8_t)n;
  o->seq = seq;
  s->outbox_count++;
}

/* Cancel the pending LEARN (queued or claimed); its BAD_PARAM is queued. */
static void cancel_learn(mxs_state_t *s)
{
  if (s->pending_learn.active) {
    outbox_push_packet(s, s->pending_learn.seq, MXP_CMD_LEARN, NULL, 0, true, MXP_ERR_BAD_PARAM);
    s->pending_learn.active = false;
  }
}

static bool function_available(const mxs_state_t *s, uint8_t fn)
{
  if (fn == MXP_FUNC_NONE) return true;
  for (int i = 0; i < s->info.function_count; i++)
    if (s->info.function_ids[i] == fn) return true;
  return false;
}

static uint32_t cached_frame_id(const mxs_state_t *s)
{
  if (s->results_len < 4) return 0;
  return (uint32_t)s->results[0] | ((uint32_t)s->results[1] << 8) | ((uint32_t)s->results[2] << 16) |
         ((uint32_t)s->results[3] << 24);
}

/* Function switch: cancel a LEARN, reset GET_RESULTS to "no records" for the
 * new function (keeping the last frame_id) and flag pending RESULTS pushes. */
static void change_function(mxs_state_t *s, uint8_t fn)
{
  if (s->function_id == fn) return;
  cancel_learn(s);
  mxp_results_t none;
  memset(&none, 0, sizeof(none));
  none.frame_id = cached_frame_id(s);
  none.function_id = fn;
  size_t n = mxp_pack_results(&none, s->results, sizeof(s->results));
  s->results_len = (uint16_t)n;
  s->results_invalidated = true;
  s->function_id = fn;
}

void mxs_set_capabilities(mxs_state_t *s, const uint8_t *ids, uint8_t n)
{
  if (n > sizeof(s->info.function_ids)) n = sizeof(s->info.function_ids);
  s->info.function_count = n;
  if (n) memcpy(s->info.function_ids, ids, n);
  if (!function_available(s, s->function_id)) change_function(s, MXP_FUNC_NONE);
}

bool mxs_take_results_invalidated(mxs_state_t *s)
{
  bool v = s->results_invalidated;
  s->results_invalidated = false;
  return v;
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
    /* Only functions that initialised (GET_INFO's list) and 0. */
    if (!function_available(s, req->payload[0])) return reply_err(req, MXP_ERR_BAD_PARAM, out, out_cap);
    change_function(s, req->payload[0]);
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
    s->pending_learn.queued_ms = now_ms;
    return MXS_DEFERRED;
  }
  case MXP_CMD_GET_RESULTS:
    return handle_get_results(s, req, out, out_cap);
  case MXP_CMD_FORGET:
    return handle_forget(s, req, out, out_cap);
  case MXP_CMD_SET_NAME:
    return handle_set_name(s, req, out, out_cap);
  case MXP_CMD_GET_LEARNED:
    return handle_get_learned(s, req, out, out_cap);
  case MXP_CMD_GET_CONFIG: {
    if (req->len != 0) return reply_err(req, MXP_ERR_BAD_LENGTH, out, out_cap);
    uint8_t c[MXP_CONFIG_SIZE];
    mxp_pack_config(&s->data->cfg, c);
    return reply(req, c, MXP_CONFIG_SIZE, out, out_cap);
  }
  case MXP_CMD_SET_CONFIG:
    return handle_set_config(s, req, out, out_cap);
  case MXP_CMD_SAVE:
  case MXP_CMD_FACTORY_RESET:
    return handle_store_op(s, req, out, out_cap);
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

bool mxs_complete_learn(mxs_state_t *s, uint8_t seq, int result)
{
  /* Only a claimed learn can be completed; a cancelled one is gone. */
  if (!s->pending_learn.active || !s->pending_learn.claimed || s->pending_learn.seq != seq) return false;
  if (result > 0 && result <= 255) {
    uint8_t id = (uint8_t)result;
    outbox_push_packet(s, seq, MXP_CMD_LEARN, &id, 1, false, 0);
  } else {
    uint8_t code = result == MXS_LEARN_FULL ? MXP_ERR_FULL : MXP_ERR_BAD_PARAM;
    outbox_push_packet(s, seq, MXP_CMD_LEARN, NULL, 0, true, code);
  }
  s->pending_learn.active = false;
  return true;
}

bool mxs_finish_learn(mxs_state_t *s, uint8_t seq, const uint8_t *rgb)
{
  if (!s->pending_learn.active || !s->pending_learn.claimed || s->pending_learn.seq != seq) return false;
  int result = MXS_LEARN_NO_COLOR;
  if (rgb) {
    mxc_learned_t before[MXST_MAX_COLORS];
    uint8_t n_before = s->data->n_colors;
    memcpy(before, s->data->colors, sizeof(before));
    int r = mxst_learn_color(s->data, rgb, s->data->cfg.tolerance);
    result = r > 0 ? r : (r == MXST_LEARN_FULL ? MXS_LEARN_FULL : MXS_LEARN_NO_COLOR);
    if (s->data->n_colors != n_before || memcmp(before, s->data->colors, sizeof(before)) != 0) mark_dirty(s);
  }
  return mxs_complete_learn(s, seq, result);
}

bool mxs_take_pending_store(mxs_state_t *s, uint8_t *cmd, uint8_t *seq, mxs_data_t *snapshot)
{
  if (!s->pending_store.active || s->pending_store.claimed) return false;
  s->pending_store.claimed = true;
  s->pending_store.gen = s->data_gen;
  *cmd = s->pending_store.cmd;
  *seq = s->pending_store.seq;
  if (snapshot) *snapshot = *s->data;
  return true;
}

bool mxs_complete_store(mxs_state_t *s, uint8_t seq, bool ok)
{
  mxs_pending_store_t *p = &s->pending_store;
  if (!p->active || !p->claimed || p->seq != seq) return false;
  outbox_push_packet(s, seq, p->cmd, NULL, 0, !ok, MXP_ERR_INTERNAL);
  if (ok && p->cmd == MXP_CMD_SAVE) {
    if (s->data_gen == p->gen) s->data->cfg.dirty = 0; /* nothing changed since the snapshot */
  } else if (ok && p->cmd == MXP_CMD_FACTORY_RESET) {
    mxst_defaults(s->data); /* dirty 0: RAM now matches the (erased) flash */
    s->data_gen++;
  }
  p->active = false;
  return true;
}

void mxs_set_results(mxs_state_t *s, const uint8_t *packed, uint16_t len)
{
  if (!packed || len == 0 || len > MXS_RESULTS_MAX) return;
  memcpy(s->results, packed, len);
  s->results_len = len;
}

size_t mxs_peek_deferred(const mxs_state_t *s, uint8_t *seq, uint8_t *out, size_t out_cap)
{
  if (s->outbox_count == 0 || out_cap < s->outbox[0].len) return 0;
  memcpy(out, s->outbox[0].pkt, s->outbox[0].len);
  *seq = s->outbox[0].seq;
  return s->outbox[0].len;
}

void mxs_ack_deferred(mxs_state_t *s)
{
  if (s->outbox_count == 0) return;
  memmove(&s->outbox[0], &s->outbox[1], sizeof(s->outbox[0]) * (size_t)(s->outbox_count - 1));
  s->outbox_count--;
}

void mxs_tick(mxs_state_t *s, uint32_t now_ms)
{
  /* Nobody claimed the LEARN (e.g. camera grabs keep failing): do not wedge
   * LEARN as BUSY until the host times out. */
  if (s->pending_learn.active && !s->pending_learn.claimed &&
      (uint32_t)(now_ms - s->pending_learn.queued_ms) > MXS_LEARN_CLAIM_TIMEOUT_MS)
    cancel_learn(s);
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

bool mxs_wants_results(const mxs_state_t *s)
{
  return s->streaming && (s->stream.flags & 2);
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
