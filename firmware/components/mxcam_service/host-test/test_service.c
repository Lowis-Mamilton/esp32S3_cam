#include <stdio.h>
#include <string.h>

#include "mxcam_service.h"

static mxs_data_t g_data; /* store data the service operates on */

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(c)) {                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
      g_fail++;                                                        \
    }                                                                  \
  } while (0)

static mxp_info_t make_info(void)
{
  mxp_info_t i;
  memset(&i, 0, sizeof(i));
  i.protocol_version = 1;
  i.fw_minor = 1;
  memcpy(i.fw_git, "abc1234", 7);
  for (int k = 0; k < 6; k++) i.serial[k] = (uint8_t)(0xA0 + k);
  i.frame_width = 320;
  i.frame_height = 240;
  i.max_results = 8;
  i.function_count = 2;
  i.function_ids[0] = 1;
  i.function_ids[1] = 2;
  return i;
}

/* Run a request through the service, decode the response with the codec. */
static size_t call(mxs_state_t *s, uint8_t cmd, uint8_t seq, const uint8_t *pl, uint16_t len,
                   uint32_t now, mxp_frame_t *resp, uint8_t *rbuf, size_t rbuf_cap)
{
  static uint8_t out[MXS_RESPONSE_MAX];
  mxp_frame_t req = { cmd, seq, len, pl };
  size_t n = mxs_handle(s, &req, now, out, sizeof(out));
  mxp_decoder_t d;
  mxp_decoder_init(&d, rbuf, rbuf_cap);
  bool got = false;
  for (size_t i = 0; i < n; i++) got = mxp_decoder_feed(&d, out[i], resp);
  if (n && !got) { printf("response did not decode\n"); g_fail++; }
  return n;
}

static void test_info(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64], expect[64];
  mxp_frame_t r;
  CHECK(call(&s, MXP_CMD_GET_INFO, 7, NULL, 0, 0, &r, rb, sizeof(rb)) > 0);
  size_t n = mxp_pack_info(&info, expect, sizeof(expect));
  CHECK(r.cmd == 0x81 && r.seq == 7);
  CHECK(r.len == n && memcmp(r.payload, expect, n) == 0);
}

static void test_ping_function(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64];
  mxp_frame_t r;
  call(&s, MXP_CMD_PING, 42, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x82 && r.len == 0 && r.seq == 42);

  uint8_t zero = 0, one = 1;
  call(&s, MXP_CMD_SET_FUNCTION, 3, &zero, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x90 && r.len == 1 && r.payload[0] == 0 && r.seq == 3);
  call(&s, MXP_CMD_SET_FUNCTION, 4, &one, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x90 && r.len == 1 && r.payload[0] == 1 && r.seq == 4 && s.function_id == 1);
  uint8_t two = 2, three = 3;
  call(&s, MXP_CMD_SET_FUNCTION, 12, &two, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x90 && r.payload[0] == 2 && s.function_id == 2);
  /* face (3) is not in make_info's list (face init failed): BAD_PARAM */
  call(&s, MXP_CMD_SET_FUNCTION, 13, &three, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.len == 2 && r.payload[0] == MXP_CMD_SET_FUNCTION &&
        r.payload[1] == MXP_ERR_BAD_PARAM && r.seq == 13 && s.function_id == 2);
  call(&s, MXP_CMD_SET_FUNCTION, 14, &zero, 1, 0, &r, rb, sizeof(rb));
  call(&s, MXP_CMD_SET_FUNCTION, 5, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_LENGTH);
  call(&s, MXP_CMD_GET_FUNCTION, 6, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x91 && r.len == 1 && r.payload[0] == 0);
}

static void test_errors(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64];
  mxp_frame_t r;
  call(&s, 0x77, 9, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 9 && r.len == 2 && r.payload[0] == 0x77 &&
        r.payload[1] == MXP_ERR_UNKNOWN_CMD);
  uint8_t junk = 5;
  call(&s, MXP_CMD_PING, 10, &junk, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[0] == MXP_CMD_PING && r.payload[1] == MXP_ERR_BAD_LENGTH);
  call(&s, MXP_CMD_GET_INFO, 11, &junk, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_LENGTH);
  /* too-small output buffer -> 0 */
  uint8_t tiny[4];
  mxp_frame_t req = { MXP_CMD_PING, 1, 0, NULL };
  CHECK(mxs_handle(&s, &req, 0, tiny, sizeof(tiny)) == 0);
}

static void start(mxs_state_t *s, uint8_t flags, uint8_t fps, uint8_t q, uint32_t now)
{
  uint8_t p[3] = { flags, fps, q };
  uint8_t rb[64];
  mxp_frame_t r;
  call(s, MXP_CMD_STREAM_START, 1, p, 3, now, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xD0 && r.len == 0);
}

static void test_stream(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  CHECK(!s.streaming);
  start(&s, 1, 0, 0, 0);
  CHECK(s.streaming && mxs_wants_frames(&s));
  CHECK(mxs_effective_fps(&s) == 10);
  CHECK(mxs_effective_quality(&s) == MXS_DEFAULT_JPEG_QUALITY);
  start(&s, 1, 15, 80, 0);
  CHECK(mxs_effective_fps(&s) == 10 && mxs_effective_quality(&s) == 80);
  start(&s, 1, 5, 0, 0);
  CHECK(mxs_effective_fps(&s) == 5);
  start(&s, 0, 5, 0, 0); /* bit0 clear: streaming state set, but no frames wanted */
  CHECK(s.streaming && !mxs_wants_frames(&s));

  uint8_t rb[64], p2[2] = { 1, 2 };
  mxp_frame_t r;
  call(&s, MXP_CMD_STREAM_START, 2, p2, 2, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_LENGTH);

  call(&s, MXP_CMD_STREAM_STOP, 3, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xD1 && r.len == 0 && !s.streaming);
}

static void test_timeout(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64];
  mxp_frame_t r;

  CHECK(!mxs_host_connected(&s, 0));
  start(&s, 1, 10, 60, 1000);
  CHECK(mxs_host_connected(&s, 1000));
  mxs_tick(&s, 3999);
  CHECK(s.streaming && mxs_host_connected(&s, 3999)); /* 2999 ms after start */
  mxs_tick(&s, 4001);
  CHECK(!s.streaming && !mxs_host_connected(&s, 4001)); /* 3001 ms */

  /* After the timeout tick, a far-future clock (>24.8 days, int32 wrap) must not
   * read the stale timestamp as a recent request. */
  start(&s, 1, 10, 60, 1000);
  mxs_tick(&s, 5000);
  CHECK(!s.streaming && !mxs_host_connected(&s, 5000));
  CHECK(!mxs_host_connected(&s, 1000u + 0x80000001u));

  /* A PING in between restarts the timer. */
  start(&s, 1, 10, 60, 10000);
  call(&s, MXP_CMD_PING, 1, NULL, 0, 12000, &r, rb, sizeof(rb));
  mxs_tick(&s, 14999);
  CHECK(s.streaming);
  mxs_tick(&s, 15001);
  CHECK(!s.streaming);

  /* now slightly before last_request_ms (cross-task clock sampling). */
  start(&s, 1, 10, 60, 1000);
  mxs_tick(&s, 999);
  CHECK(s.streaming && mxs_host_connected(&s, 999));

  /* uint32 wraparound of the millisecond clock. */
  start(&s, 1, 10, 60, 0xFFFFFF00u);
  mxs_tick(&s, 100);
  CHECK(s.streaming);
  mxs_tick(&s, 0xFFFFFF00u + 3001u);
  CHECK(!s.streaming);
}

/* ---- deferred LEARN ---- */
static void learn_payload(uint8_t func, uint8_t *p)
{
  p[0] = func;
  uint16_t v[4] = { 100, 80, 20, 20 };
  for (int i = 0; i < 4; i++) {
    p[1 + 2 * i] = (uint8_t)v[i];
    p[2 + 2 * i] = (uint8_t)(v[i] >> 8);
  }
}

static void decode(const uint8_t *out, size_t n, mxp_frame_t *r, uint8_t *rb, size_t cap)
{
  mxp_decoder_t d;
  mxp_decoder_init(&d, rb, cap);
  bool got = false;
  for (size_t i = 0; i < n; i++) got = mxp_decoder_feed(&d, out[i], r);
  CHECK(got);
}

/* Pops the front deferred response packet (as the link flush does). */
static size_t pop_deferred(mxs_state_t *s, uint8_t *out, size_t cap)
{
  uint8_t seq = 0;
  size_t n = mxs_peek_deferred(s, &seq, out, cap);
  if (n > 0) mxs_ack_deferred(s);
  return n;
}

static size_t learn(mxs_state_t *s, uint8_t func, uint8_t seq, uint32_t now, mxp_frame_t *r,
                    uint8_t *rb, size_t rb_cap)
{
  uint8_t p[9], out[64];
  learn_payload(func, p);
  mxp_frame_t req = { MXP_CMD_LEARN, seq, 9, p };
  size_t n = mxs_handle(s, &req, now, out, sizeof(out));
  if (n != MXS_DEFERRED && n > 0) decode(out, n, r, rb, rb_cap);
  return n;
}

static void test_learn(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64], out[64];
  mxp_frame_t r;

  /* active function is not colour -> BAD_PARAM immediately */
  CHECK(learn(&s, 1, 1, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(r.cmd == 0xFF && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM && r.seq == 1);

  s.function_id = 1;
  /* wrong function in request */
  CHECK(learn(&s, 2, 2, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_PARAM && r.seq == 2);
  /* bad length */
  uint8_t p[9] = { 1 };
  mxp_frame_t bad = { MXP_CMD_LEARN, 3, 5, p };
  size_t n = mxs_handle(&s, &bad, 0, out, sizeof(out));
  CHECK(n != MXS_DEFERRED && n > 0);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_LENGTH);
  CHECK(!s.pending_learn.active);

  /* valid -> DEFERRED, seq remembered */
  CHECK(learn(&s, 1, 9, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(s.pending_learn.active && s.pending_learn.seq == 9);
  /* second while pending -> BUSY */
  CHECK(learn(&s, 1, 10, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BUSY && r.seq == 10);

  /* take / complete */
  uint8_t seq = 0;
  mxp_learn_req_t req;
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  CHECK(seq == 9 && req.function_id == 1 && req.x == 100 && req.y == 80 && req.w == 20 && req.h == 20);
  CHECK(!mxs_take_pending_learn(&s, &seq, &req)); /* claimed once */
  /* still busy while the pipeline works on it */
  CHECK(learn(&s, 1, 11, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && r.payload[1] == MXP_ERR_BUSY);
  CHECK(mxs_complete_learn(&s, seq, 4));
  n = pop_deferred(&s, out, sizeof(out));
  CHECK(n > 0);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 9 && r.len == 1 && r.payload[0] == 4);
  CHECK(!s.pending_learn.active);

  /* NO_COLOR -> BAD_PARAM, FULL -> FULL */
  CHECK(learn(&s, 1, 20, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  CHECK(mxs_complete_learn(&s, seq, MXS_LEARN_NO_COLOR));
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 20 && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM);
  CHECK(learn(&s, 1, 21, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  CHECK(mxs_complete_learn(&s, seq, MXS_LEARN_FULL));
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 21 && r.payload[1] == MXP_ERR_FULL);
  CHECK(!s.pending_learn.active);
  CHECK(!mxs_complete_learn(&s, 21, 1)); /* nothing pending */
}

/* Pop the front deferred packet and decode it. */
static void pop_decoded(mxs_state_t *s, mxp_frame_t *r, uint8_t *rb, size_t cap)
{
  uint8_t out[MXS_DEFERRED_PKT], seq = 0;
  size_t n = mxs_peek_deferred(s, &seq, out, sizeof(out));
  CHECK(n > 0);
  decode(out, n, r, rb, cap);
  mxs_ack_deferred(s);
}

static void test_learn_cancel(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  uint8_t rb[64], out[64], seq = 0;
  mxp_frame_t r;
  uint8_t zero = 0;

  /* SET_FUNCTION change cancels an unclaimed learn; the response carries
   * BAD_PARAM and the original seq. */
  s.function_id = 1;
  CHECK(learn(&s, 1, 31, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  call(&s, MXP_CMD_SET_FUNCTION, 6, &zero, 1, 10, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x90);
  CHECK(!s.pending_learn.active);
  size_t n = mxs_peek_deferred(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 31);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 31 && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM);
  /* peek does not consume: a failed send is retried by the next flush */
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == n && seq == 31);
  mxs_ack_deferred(&s);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);

  /* stream/host timeout cancels it too */
  s.function_id = 1;
  CHECK(learn(&s, 1, 32, 1000, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  mxp_learn_req_t req;
  CHECK(mxs_take_pending_learn(&s, &seq, &req)); /* claimed: only the host timeout applies */
  mxs_tick(&s, 3999);
  CHECK(s.pending_learn.active);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);
  mxs_tick(&s, 4001);
  CHECK(!s.pending_learn.active);
  n = mxs_peek_deferred(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 32);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 32 && r.payload[1] == MXP_ERR_BAD_PARAM);
  mxs_ack_deferred(&s);

  /* a CLAIMED learn is cancelled too (BAD_PARAM queued), so it cannot wedge
   * LEARN as BUSY; a new LEARN is accepted afterwards. */
  s.function_id = 1;
  CHECK(learn(&s, 1, 33, 5000, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  call(&s, MXP_CMD_SET_FUNCTION, 7, &zero, 1, 5001, &r, rb, sizeof(rb));
  CHECK(!s.pending_learn.active);
  n = mxs_peek_deferred(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 33);
  mxs_ack_deferred(&s);
  CHECK(!mxs_complete_learn(&s, 33, 2)); /* late completion is ignored */
  s.function_id = 1;
  CHECK(learn(&s, 1, 34, 5002, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(!mxs_complete_learn(&s, 34, 2)); /* not claimed yet */
  CHECK(s.pending_learn.active);

  /* an unclaimed LEARN is cancelled after 1000 ms (BAD_PARAM), a claimed one is not */
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  s.function_id = 1;
  CHECK(learn(&s, 1, 50, 10000, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  mxs_tick(&s, 11000); /* exactly 1000 ms: still waiting */
  CHECK(s.pending_learn.active);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);
  mxs_tick(&s, 11001);
  CHECK(!s.pending_learn.active);
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 50 && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM);
  CHECK(learn(&s, 1, 51, 11002, &r, rb, sizeof(rb)) == MXS_DEFERRED); /* not BUSY any more */
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  mxs_tick(&s, 12500); /* claimed: the pipeline completes it */
  CHECK(s.pending_learn.active);

  /* two cancels before a flush are both delivered, in order */
  mxst_defaults(&g_data);
  mxs_init(&s, &info, &g_data);
  s.function_id = 1;
  CHECK(learn(&s, 1, 40, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  call(&s, MXP_CMD_SET_FUNCTION, 8, &zero, 1, 1, &r, rb, sizeof(rb));
  s.function_id = 1;
  CHECK(learn(&s, 1, 41, 2, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  call(&s, MXP_CMD_SET_FUNCTION, 9, &zero, 1, 3, &r, rb, sizeof(rb));
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) > 0 && seq == 40);
  mxs_ack_deferred(&s);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) > 0 && seq == 41);
  mxs_ack_deferred(&s);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);
}

/* ---- M2 commands ---- */
static mxs_state_t *fresh(mxs_state_t *s)
{
  mxp_info_t info = make_info();
  mxst_defaults(&g_data);
  mxs_init(s, &info, &g_data);
  return s;
}

static uint8_t g_rb[MXS_RESPONSE_MAX];
static mxp_frame_t g_r;
/* call() with the shared response buffer; returns &g_r. */
static const mxp_frame_t *req(mxs_state_t *s, uint8_t cmd, uint8_t seq, const uint8_t *pl, uint16_t len)
{
  memset(&g_r, 0, sizeof(g_r));
  call(s, cmd, seq, pl, len, 0, &g_r, g_rb, sizeof(g_rb));
  return &g_r;
}
static bool is_err(const mxp_frame_t *r, uint8_t cmd, uint8_t code)
{
  return r->cmd == 0xFF && r->len == 2 && r->payload[0] == cmd && r->payload[1] == code;
}
static void add_color(uint8_t id, uint8_t r, uint8_t g, uint8_t b)
{
  mxc_learned_t *c = &g_data.colors[g_data.n_colors++];
  c->id = id;
  c->rgb[0] = r;
  c->rgb[1] = g;
  c->rgb[2] = b;
}

static void test_results(void)
{
  mxs_state_t s;
  fresh(&s);
  const mxp_frame_t *r;
  uint8_t two = 2, one = 1;
  req(&s, MXP_CMD_SET_FUNCTION, 1, &two, 1);
  /* before any processed frame: frame 0, current function, no records */
  r = req(&s, MXP_CMD_GET_RESULTS, 5, NULL, 0);
  CHECK(r->cmd == 0xA0 && r->seq == 5 && r->len == 6);
  mxp_results_t res;
  CHECK(mxp_unpack_results(r->payload, r->len, &res) && res.frame_id == 0 && res.function_id == 2 &&
        res.count == 0);
  r = req(&s, MXP_CMD_GET_RESULTS, 6, &one, 1);
  CHECK(is_err(r, MXP_CMD_GET_RESULTS, MXP_ERR_BAD_LENGTH));

  /* latest processed frame, byte for byte */
  mxp_results_t in;
  memset(&in, 0, sizeof(in));
  in.frame_id = 1234;
  in.function_id = 2;
  in.count = MXP_MAX_RESULTS;
  for (int i = 0; i < MXP_MAX_RESULTS; i++) {
    in.r[i].id = (uint16_t)(100 + i);
    in.r[i].x = (int16_t)(10 * i);
    in.r[i].w = 20;
    in.r[i].confidence = 90;
    in.r[i].extra_len = MXP_RESULT_EXTRA_MAX;
    in.r[i].extra[0] = (uint8_t)i;
  }
  uint8_t packed[MXS_RESULTS_MAX + 1];
  size_t n = mxp_pack_results(&in, packed, sizeof(packed));
  CHECK(n == MXS_RESULTS_MAX); /* the worst case fits exactly */
  mxs_set_results(&s, packed, (uint16_t)n);
  r = req(&s, MXP_CMD_GET_RESULTS, 7, NULL, 0);
  CHECK(r->cmd == 0xA0 && r->len == n && memcmp(r->payload, packed, n) == 0);
  in.frame_id = 1235;
  in.count = 1;
  n = mxp_pack_results(&in, packed, sizeof(packed));
  mxs_set_results(&s, packed, (uint16_t)n);
  r = req(&s, MXP_CMD_GET_RESULTS, 8, NULL, 0);
  CHECK(r->len == n && mxp_unpack_results(r->payload, r->len, &res) && res.frame_id == 1235 && res.count == 1);
  /* oversized input is ignored (keeps the previous results) */
  mxs_set_results(&s, packed, MXS_RESULTS_MAX + 1);
  r = req(&s, MXP_CMD_GET_RESULTS, 9, NULL, 0);
  CHECK(r->len == n);

  /* RESULTS pushes need streaming + flags bit1 */
  CHECK(!mxs_wants_results(&s));
  start(&s, 1, 10, 0, 0);
  CHECK(!mxs_wants_results(&s));
  start(&s, 2, 10, 0, 0);
  CHECK(mxs_wants_results(&s) && !mxs_wants_frames(&s));
  start(&s, 3, 10, 0, 0);
  CHECK(mxs_wants_results(&s) && mxs_wants_frames(&s));
  req(&s, MXP_CMD_STREAM_STOP, 10, NULL, 0);
  CHECK(!mxs_wants_results(&s));
}

static void forget(mxs_state_t *s, uint8_t fn, uint16_t id, uint8_t seq)
{
  uint8_t p[3] = { fn, (uint8_t)id, (uint8_t)(id >> 8) };
  req(s, MXP_CMD_FORGET, seq, p, 3);
}

static void test_forget(void)
{
  mxs_state_t s;
  fresh(&s);
  add_color(1, 200, 0, 0);
  add_color(2, 0, 200, 0);
  add_color(3, 0, 0, 200);
  mxst_set_name(&g_data, 1, 2, "green", 5);
  mxst_set_name(&g_data, 2, 7, "door", 4);
  mxst_set_name(&g_data, 2, 9, "exit", 4);

  uint8_t p[3] = { 1, 2, 0 };
  CHECK(is_err(req(&s, MXP_CMD_FORGET, 1, p, 2), MXP_CMD_FORGET, MXP_ERR_BAD_LENGTH));
  CHECK(g_data.cfg.dirty == 0);
  forget(&s, 1, 2, 2);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0 && g_r.seq == 2);
  CHECK(g_data.n_colors == 2 && g_data.colors[0].id == 1 && g_data.colors[1].id == 3);
  CHECK(mxst_get_name(&g_data, 1, 2, NULL) == NULL && g_data.cfg.dirty == 1);
  /* idempotent: absent ids succeed */
  forget(&s, 1, 2, 3);
  CHECK(g_r.cmd == 0xB1);
  forget(&s, 1, 500, 4);
  CHECK(g_r.cmd == 0xB1 && g_data.n_colors == 2);
  /* tag names: one, then all */
  forget(&s, 2, 7, 5);
  CHECK(g_r.cmd == 0xB1 && mxst_get_name(&g_data, 2, 7, NULL) == NULL &&
        mxst_get_name(&g_data, 2, 9, NULL) != NULL);
  forget(&s, 2, 0xFFFF, 6);
  CHECK(g_r.cmd == 0xB1 && g_data.n_names == 0);
  /* all colours */
  forget(&s, 1, 0xFFFF, 7);
  CHECK(g_r.cmd == 0xB1 && g_data.n_colors == 0);
  /* unsupported functions */
  forget(&s, 0, 1, 8);
  CHECK(is_err(&g_r, MXP_CMD_FORGET, MXP_ERR_BAD_PARAM));
  /* face (3): nothing is learned or named, so FORGET succeeds (see test_face_function);
   * line (4) and object (5) see test_forget_line_resets_mode / test_forget_object_and_name;
   * QR (6), shapes (11) and motion (12) see the M7 tests */
  forget(&s, 7, 1, 9);
  CHECK(is_err(&g_r, MXP_CMD_FORGET, MXP_ERR_BAD_PARAM));
}

static const mxp_frame_t *set_name(mxs_state_t *s, uint8_t fn, uint16_t id, const char *name, uint8_t n)
{
  uint8_t p[4 + 64];
  p[0] = fn;
  p[1] = (uint8_t)id;
  p[2] = (uint8_t)(id >> 8);
  p[3] = n;
  memcpy(p + 4, name, n);
  return req(s, MXP_CMD_SET_NAME, 3, p, (uint16_t)(4 + n));
}

static void test_set_name(void)
{
  mxs_state_t s;
  fresh(&s);
  add_color(2, 0, 200, 0);
  /* colour: id must be learned */
  CHECK(is_err(set_name(&s, 1, 1, "red", 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(g_data.n_names == 0 && g_data.cfg.dirty == 0);
  CHECK(set_name(&s, 1, 2, "\xE7\xB6\xA0", 3)->cmd == 0xB2 && g_r.len == 0);
  uint8_t l = 0;
  const char *nm = mxst_get_name(&g_data, 1, 2, &l);
  CHECK(nm && l == 3 && memcmp(nm, "\xE7\xB6\xA0", 3) == 0 && g_data.cfg.dirty == 1);
  /* AprilTag: any 0..586 id, no learning needed */
  CHECK(set_name(&s, 2, 0, "zero", 4)->cmd == 0xB2);
  CHECK(set_name(&s, 2, 586, "last", 4)->cmd == 0xB2);
  CHECK(is_err(set_name(&s, 2, 587, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  /* other functions */
  CHECK(is_err(set_name(&s, 0, 1, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 3, 1, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  /* name length: 48 ok, 49 BAD_LENGTH; mismatch BAD_LENGTH; bad UTF-8 BAD_PARAM */
  char big[64];
  memset(big, 'n', sizeof(big));
  CHECK(set_name(&s, 2, 5, big, 48)->cmd == 0xB2);
  CHECK(is_err(set_name(&s, 2, 5, big, 49), MXP_CMD_SET_NAME, MXP_ERR_BAD_LENGTH));
  uint8_t p[8] = { 2, 5, 0, 3, 'a', 'b' };
  CHECK(is_err(req(&s, MXP_CMD_SET_NAME, 4, p, 6), MXP_CMD_SET_NAME, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(req(&s, MXP_CMD_SET_NAME, 4, p, 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(set_name(&s, 2, 5, "\xC3\x28", 2), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 2, 5, "\xED\xA0\x80", 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  /* name_len 0 clears */
  CHECK(set_name(&s, 2, 5, "", 0)->cmd == 0xB2 && mxst_get_name(&g_data, 2, 5, NULL) == NULL);
  /* table full -> FULL */
  fresh(&s);
  for (uint16_t i = 0; i < MXST_MAX_NAMES; i++) CHECK(set_name(&s, 2, i, "t", 1)->cmd == 0xB2);
  CHECK(is_err(set_name(&s, 2, 100, "t", 1), MXP_CMD_SET_NAME, MXP_ERR_FULL));
  CHECK(set_name(&s, 2, 3, "again", 5)->cmd == 0xB2); /* replacing still works */
}

static void test_get_learned(void)
{
  mxs_state_t s;
  fresh(&s);
  uint8_t fn = 1;
  /* empty */
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_LEARNED, 1, &fn, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 && r->payload[0] == 0);
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED, 2, NULL, 0), MXP_CMD_GET_LEARNED, MXP_ERR_BAD_LENGTH));
  uint8_t bad = 3;
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED, 2, &bad, 1), MXP_CMD_GET_LEARNED, MXP_ERR_BAD_PARAM));
  bad = 0;
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED, 2, &bad, 1), MXP_CMD_GET_LEARNED, MXP_ERR_BAD_PARAM));

  /* colours, by id, with rgb and names */
  add_color(1, 200, 10, 20);
  add_color(4, 30, 40, 220);
  mxst_set_name(&g_data, 1, 4, "blue", 4);
  mxst_set_name(&g_data, 2, 9, "tag9", 4);
  r = req(&s, MXP_CMD_GET_LEARNED, 3, &fn, 1);
  static const uint8_t want1[] = { 2, 1, 0, 200, 10, 20, 0, 4, 0, 30, 40, 220, 4, 'b', 'l', 'u', 'e' };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want1) && memcmp(r->payload, want1, sizeof(want1)) == 0);

  /* tags: named ids only, sorted, rgb 0 */
  mxst_set_name(&g_data, 2, 300, "far", 3);
  mxst_set_name(&g_data, 2, 2, "near", 4);
  fn = 2;
  r = req(&s, MXP_CMD_GET_LEARNED, 4, &fn, 1);
  static const uint8_t want2[] = { 3, 2, 0, 0, 0, 0, 4, 'n', 'e', 'a', 'r', 9, 0, 0, 0, 0, 4, 't', 'a', 'g', '9',
                                   0x2C, 0x01, 0, 0, 0, 3, 'f', 'a', 'r' };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want2) && memcmp(r->payload, want2, sizeof(want2)) == 0);

  /* worst case: 32 tag names of 48 bytes fits in one response */
  fresh(&s);
  char big[MXP_NAME_MAX];
  memset(big, 'x', sizeof(big));
  for (uint16_t i = 0; i < MXST_MAX_NAMES; i++) mxst_set_name(&g_data, 2, (uint16_t)(586 - i), big, MXP_NAME_MAX);
  r = req(&s, MXP_CMD_GET_LEARNED, 5, &fn, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 + MXST_MAX_NAMES * (6 + MXP_NAME_MAX) && r->payload[0] == MXST_MAX_NAMES);
  CHECK(r->payload[1] == (uint8_t)(586 - 31) && r->payload[2] == (uint8_t)((586 - 31) >> 8));
}

static void test_config(void)
{
  mxs_state_t s;
  fresh(&s);
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_CONFIG, 1, NULL, 0);
  uint8_t def[MXP_CONFIG_SIZE];
  mxp_pack_config(&g_data.cfg, def);
  CHECK(r->cmd == 0xC0 && r->len == MXP_CONFIG_SIZE && memcmp(r->payload, def, MXP_CONFIG_SIZE) == 0);
  CHECK(r->payload[15] == 0); /* not dirty */
  uint8_t junk = 0;
  CHECK(is_err(req(&s, MXP_CMD_GET_CONFIG, 2, &junk, 1), MXP_CMD_GET_CONFIG, MXP_ERR_BAD_LENGTH));

  /* SET_CONFIG clamps / falls back and answers with the applied config */
  uint8_t c[MXP_CONFIG_SIZE];
  memcpy(c, def, sizeof(c));
  c[1] = 200;                                      /* tolerance -> 100 */
  c[2] = 0x34; c[3] = 0x12;                        /* min_area 0x1234 */
  c[4] = 0;                                        /* color.max_results -> 1 */
  c[9] = 0x10; c[10] = 0x27; c[11] = 0; c[12] = 0; /* baud 10000 -> 115200 */
  c[13] = 0x80;                                    /* i2c -> 0x5A */
  c[14] = 70;                                      /* led */
  c[15] = 0;                                       /* dirty is read-only */
  r = req(&s, MXP_CMD_SET_CONFIG, 3, c, sizeof(c));
  CHECK(r->cmd == 0xC1 && r->len == MXP_CONFIG_SIZE);
  CHECK(r->payload[1] == 100 && r->payload[2] == 0x34 && r->payload[3] == 0x12 && r->payload[4] == 1);
  CHECK(r->payload[9] == 0x00 && r->payload[10] == 0xC2 && r->payload[11] == 0x01); /* 115200 */
  CHECK(r->payload[13] == 0x5A && r->payload[14] == 70 && r->payload[15] == 1);
  CHECK(g_data.cfg.tolerance == 100 && g_data.cfg.min_area == 0x1234 && g_data.cfg.led == 70 &&
        g_data.cfg.dirty == 1);
  r = req(&s, MXP_CMD_GET_CONFIG, 4, NULL, 0);
  CHECK(r->cmd == 0xC0 && r->payload[15] == 1 && r->payload[1] == 100);

  /* version != 1 rejects the whole packet; wrong length is BAD_LENGTH */
  c[0] = 2;
  c[1] = 5;
  CHECK(is_err(req(&s, MXP_CMD_SET_CONFIG, 5, c, sizeof(c)), MXP_CMD_SET_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(g_data.cfg.tolerance == 100);
  c[0] = 1;
  CHECK(is_err(req(&s, MXP_CMD_SET_CONFIG, 6, c, 17), MXP_CMD_SET_CONFIG, MXP_ERR_BAD_LENGTH));
}

static size_t deferred(mxs_state_t *s, uint8_t cmd, uint8_t seq)
{
  static uint8_t out[MXS_RESPONSE_MAX];
  mxp_frame_t rq = { cmd, seq, 0, NULL };
  return mxs_handle(s, &rq, 0, out, sizeof(out));
}

static void test_save_reset(void)
{
  mxs_state_t s;
  fresh(&s);
  static mxs_data_t snap;
  uint8_t out[64], cmd = 0, seq = 0;

  /* SAVE is deferred to the save worker; a second store op is BUSY */
  CHECK(!mxs_take_pending_store(&s, &cmd, &seq, &snap));
  add_color(1, 200, 0, 0);
  g_data.cfg.dirty = 1;
  CHECK(deferred(&s, MXP_CMD_SAVE, 40) == MXS_DEFERRED);
  CHECK(is_err(req(&s, MXP_CMD_SAVE, 41, NULL, 0), MXP_CMD_SAVE, MXP_ERR_BUSY));
  CHECK(is_err(req(&s, MXP_CMD_FACTORY_RESET, 42, NULL, 0), MXP_CMD_FACTORY_RESET, MXP_ERR_BUSY));
  uint8_t junk = 1;
  CHECK(is_err(req(&s, MXP_CMD_SAVE, 43, &junk, 1), MXP_CMD_SAVE, MXP_ERR_BAD_LENGTH));
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap));
  CHECK(cmd == MXP_CMD_SAVE && seq == 40 && snap.n_colors == 1 && snap.colors[0].id == 1);
  CHECK(!mxs_take_pending_store(&s, &cmd, &seq, &snap)); /* claimed once */
  CHECK(!mxs_complete_store(&s, 39, true)); /* wrong seq */
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);
  CHECK(mxs_complete_store(&s, 40, true));
  mxp_frame_t r;
  uint8_t rb[64];
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC2 && r.seq == 40 && r.len == 0);
  CHECK(g_data.cfg.dirty == 0);
  CHECK(!mxs_complete_store(&s, 40, true)); /* only once */
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == 0);

  /* a change made while the save was running keeps dirty set */
  CHECK(deferred(&s, MXP_CMD_SAVE, 50) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap));
  forget(&s, 1, 1, 51);
  CHECK(g_data.cfg.dirty == 1);
  CHECK(mxs_complete_store(&s, 50, true));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC2 && g_data.cfg.dirty == 1);

  /* a failed save answers INTERNAL and keeps dirty */
  CHECK(deferred(&s, MXP_CMD_SAVE, 52) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap));
  CHECK(mxs_complete_store(&s, 52, false));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 52 && r.payload[0] == MXP_CMD_SAVE && r.payload[1] == MXP_ERR_INTERNAL);
  CHECK(g_data.cfg.dirty == 1);

  /* FACTORY_RESET: defaults only after the erase succeeded */
  add_color(3, 0, 0, 200);
  mxst_set_name(&g_data, 2, 4, "x", 1);
  g_data.cfg.tolerance = 90;
  CHECK(deferred(&s, MXP_CMD_FACTORY_RESET, 60) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap) && cmd == MXP_CMD_FACTORY_RESET && seq == 60);
  CHECK(mxs_complete_store(&s, 60, false));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[0] == MXP_CMD_FACTORY_RESET && r.payload[1] == MXP_ERR_INTERNAL);
  CHECK(g_data.n_colors == 1 && g_data.cfg.tolerance == 90);
  CHECK(deferred(&s, MXP_CMD_FACTORY_RESET, 61) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap));
  CHECK(mxs_complete_store(&s, 61, true));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC3 && r.seq == 61 && r.len == 0);
  mxs_data_t def;
  mxst_defaults(&def);
  CHECK(g_data.n_colors == 0 && g_data.n_names == 0 && memcmp(&g_data.cfg, &def.cfg, sizeof(def.cfg)) == 0);
  /* the store slot is free again */
  CHECK(deferred(&s, MXP_CMD_SAVE, 62) == MXS_DEFERRED);
}

static void test_finish_learn(void)
{
  mxs_state_t s;
  fresh(&s);
  uint8_t out[64], rb[64], seq = 0, one = 1, zero = 0;
  mxp_frame_t r;
  mxp_learn_req_t lr;
  const uint8_t red[3] = { 230, 20, 20 }, green[3] = { 20, 200, 40 };
  req(&s, MXP_CMD_SET_FUNCTION, 1, &one, 1);

  CHECK(learn(&s, 1, 70, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(!mxs_finish_learn(&s, 70, red)); /* not claimed yet */
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, red));
  size_t n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 70 && r.len == 1 && r.payload[0] == 1);
  CHECK(g_data.n_colors == 1 && g_data.cfg.dirty == 1);

  /* same colour again -> same id */
  g_data.cfg.dirty = 0;
  CHECK(learn(&s, 1, 71, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, red));
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  /* identical rgb: nothing changed, so not dirty */
  CHECK(r.cmd == 0xB0 && r.payload[0] == 1 && g_data.n_colors == 1 && g_data.cfg.dirty == 0);

  /* no colour in the region -> BAD_PARAM, data untouched */
  g_data.cfg.dirty = 0;
  CHECK(learn(&s, 1, 72, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, NULL));
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 72 && g_data.cfg.dirty == 0);

  /* cancelled while sampling: nothing learned, nothing sent */
  CHECK(learn(&s, 1, 73, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  req(&s, MXP_CMD_SET_FUNCTION, 2, &zero, 1);
  CHECK(!mxs_finish_learn(&s, seq, green));
  CHECK(g_data.n_colors == 1);
  n = pop_deferred(&s, out, sizeof(out)); /* only the cancel's BAD_PARAM */
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 73);
  CHECK(pop_deferred(&s, out, sizeof(out)) == 0);

  /* full -> FULL */
  req(&s, MXP_CMD_SET_FUNCTION, 3, &one, 1);
  g_data.n_colors = 1;
  for (uint8_t id = 2; id <= 7; id++) add_color(id, 0, 0, 0); /* placeholders: black never matches */
  CHECK(learn(&s, 1, 74, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, green));
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_FULL) && r.seq == 74);
}

static void test_capabilities(void)
{
  mxs_state_t s;
  fresh(&s);
  uint8_t one = 1, two = 2, zero = 0;
  /* only colour works (e.g. AprilTag init failed) */
  const uint8_t only_color[1] = { 1 };
  mxs_set_capabilities(&s, only_color, 1);
  CHECK(is_err(req(&s, MXP_CMD_SET_FUNCTION, 1, &two, 1), MXP_CMD_SET_FUNCTION, MXP_ERR_BAD_PARAM));
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 2, &one, 1)->cmd == 0x90 && s.function_id == 1);
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_INFO, 3, NULL, 0);
  CHECK(r->cmd == 0x81 && r->len == 24 && r->payload[22] == 1 && r->payload[23] == 1);
  /* nothing works (no pipeline): the current function falls back to 0, LEARN is refused at once */
  mxs_set_capabilities(&s, NULL, 0);
  CHECK(s.function_id == 0);
  CHECK(is_err(req(&s, MXP_CMD_SET_FUNCTION, 4, &one, 1), MXP_CMD_SET_FUNCTION, MXP_ERR_BAD_PARAM));
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 5, &zero, 1)->cmd == 0x90);
  uint8_t lp[9] = { 1, 0, 0, 0, 0, 10, 0, 10, 0 };
  CHECK(is_err(req(&s, MXP_CMD_LEARN, 6, lp, 9), MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
  r = req(&s, MXP_CMD_GET_INFO, 7, NULL, 0);
  CHECK(r->len == 23 && r->payload[22] == 0);
}

static void test_results_on_function_change(void)
{
  mxs_state_t s;
  fresh(&s);
  uint8_t one = 1, two = 2;
  req(&s, MXP_CMD_SET_FUNCTION, 1, &two, 1);
  CHECK(mxs_take_results_invalidated(&s)); /* 0 -> 2 is a change */
  CHECK(!mxs_take_results_invalidated(&s));
  mxp_results_t in;
  memset(&in, 0, sizeof(in));
  in.frame_id = 777;
  in.function_id = 2;
  in.count = 1;
  in.r[0].id = 5;
  uint8_t packed[MXS_RESULTS_MAX];
  size_t n = mxp_pack_results(&in, packed, sizeof(packed));
  mxs_set_results(&s, packed, (uint16_t)n);
  /* same function again: cache kept */
  req(&s, MXP_CMD_SET_FUNCTION, 2, &two, 1);
  CHECK(!mxs_take_results_invalidated(&s));
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_RESULTS, 3, NULL, 0);
  CHECK(r->len == n);
  /* change: count 0, new function, last frame_id kept; pending pushes must go */
  req(&s, MXP_CMD_SET_FUNCTION, 4, &one, 1);
  CHECK(mxs_take_results_invalidated(&s));
  r = req(&s, MXP_CMD_GET_RESULTS, 5, NULL, 0);
  mxp_results_t res;
  CHECK(r->cmd == 0xA0 && mxp_unpack_results(r->payload, r->len, &res) && res.frame_id == 777 &&
        res.function_id == 1 && res.count == 0);
}

/* Face detection (function 3): selectable only when it initialised (in
 * GET_INFO's list); nothing to learn or name; FORGET succeeds (nothing to
 * forget) without touching the data. */
static void test_face_function(void)
{
  mxs_state_t s;
  fresh(&s); /* capabilities 1, 2: face init failed */
  uint8_t three = 3, rb[64];
  mxp_frame_t r;
  CHECK(is_err(req(&s, MXP_CMD_SET_FUNCTION, 1, &three, 1), MXP_CMD_SET_FUNCTION, MXP_ERR_BAD_PARAM));
  CHECK(s.function_id == 0);

  const uint8_t all[3] = { 1, 2, 3 };
  mxs_set_capabilities(&s, all, 3);
  const mxp_frame_t *g = req(&s, MXP_CMD_GET_INFO, 2, NULL, 0);
  CHECK(g->cmd == 0x81 && g->len == 26 && g->payload[22] == 3 && g->payload[23] == 1 && g->payload[24] == 2 &&
        g->payload[25] == 3);
  g = req(&s, MXP_CMD_SET_FUNCTION, 3, &three, 1);
  CHECK(g->cmd == 0x90 && g->len == 1 && g->payload[0] == 3 && s.function_id == 3);
  CHECK(req(&s, MXP_CMD_GET_FUNCTION, 4, NULL, 0)->payload[0] == 3);

  /* LEARN for face (even with face current) -> BAD_PARAM at once, nothing pending */
  CHECK(learn(&s, 3, 5, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && !s.pending_learn.active);
  /* SET_NAME for face -> BAD_PARAM (also clearing a name), nothing stored */
  CHECK(is_err(set_name(&s, 3, 0, "me", 2), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 3, 0, "", 0), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(g_data.n_names == 0 && g_data.cfg.dirty == 0);
  /* FORGET for face -> success, data untouched (not dirty) */
  add_color(1, 200, 0, 0);
  mxst_set_name(&g_data, 2, 7, "door", 4);
  forget(&s, 3, 0, 6);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0 && g_r.seq == 6);
  forget(&s, 3, 0xFFFF, 7);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0);
  CHECK(g_data.n_colors == 1 && g_data.n_names == 1 && g_data.cfg.dirty == 0);
  CHECK(is_err(req(&s, MXP_CMD_FORGET, 8, &three, 1), MXP_CMD_FORGET, MXP_ERR_BAD_LENGTH));

  /* face results are cached like the others */
  mxp_results_t in;
  memset(&in, 0, sizeof(in));
  in.frame_id = 42;
  in.function_id = 3;
  in.count = 1;
  in.r[0].x = 160;
  in.r[0].y = 120;
  in.r[0].w = 60;
  in.r[0].h = 80;
  in.r[0].confidence = 92;
  uint8_t packed[MXS_RESULTS_MAX];
  size_t n = mxp_pack_results(&in, packed, sizeof(packed));
  mxs_set_results(&s, packed, (uint16_t)n);
  g = req(&s, MXP_CMD_GET_RESULTS, 9, NULL, 0);
  mxp_results_t out;
  CHECK(g->cmd == 0xA0 && mxp_unpack_results(g->payload, g->len, &out) && out.frame_id == 42 &&
        out.function_id == 3 && out.count == 1 && out.r[0].id == 0 && out.r[0].confidence == 92);

  /* face no longer available: falls back to 0 */
  mxs_set_capabilities(&s, all, 2);
  CHECK(s.function_id == 0);
  CHECK(is_err(req(&s, MXP_CMD_SET_FUNCTION, 10, &three, 1), MXP_CMD_SET_FUNCTION, MXP_ERR_BAD_PARAM));
}

static void test_dirty_only_on_change(void)
{
  mxs_state_t s;
  fresh(&s);
  add_color(1, 200, 0, 0);
  forget(&s, 1, 5, 1); /* absent colour */
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  forget(&s, 2, 9, 2); /* absent tag name */
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  uint8_t c[MXP_CONFIG_SIZE];
  mxp_pack_config(&g_data.cfg, c);
  CHECK(req(&s, MXP_CMD_SET_CONFIG, 3, c, sizeof(c))->cmd == 0xC1 && g_data.cfg.dirty == 0); /* identical */
  CHECK(g_r.payload[15] == 0);
  CHECK(set_name(&s, 2, 4, "", 0)->cmd == 0xB2 && g_data.cfg.dirty == 0); /* clearing an absent name */
  CHECK(set_name(&s, 2, 4, "door", 4)->cmd == 0xB2 && g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  CHECK(set_name(&s, 2, 4, "door", 4)->cmd == 0xB2 && g_data.cfg.dirty == 0); /* same name */
  c[1] = 31;
  CHECK(req(&s, MXP_CMD_SET_CONFIG, 4, c, sizeof(c))->cmd == 0xC1 && g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 1, 0xFFFF, 5);
  CHECK(g_data.cfg.dirty == 1 && g_data.n_colors == 0);
  g_data.cfg.dirty = 0;
  forget(&s, 1, 0xFFFF, 6); /* nothing left */
  CHECK(g_data.cfg.dirty == 0);
}

static void test_deferred_order(void)
{
  mxs_state_t s;
  fresh(&s);
  uint8_t one = 1, zero = 0, seq = 0, out[64], rb[64];
  mxp_frame_t r;
  mxp_learn_req_t lr;
  const uint8_t red[3] = { 230, 20, 20 };
  /* a learn result and a cancel queue up in order; nothing is lost if not flushed */
  req(&s, MXP_CMD_SET_FUNCTION, 1, &one, 1);
  CHECK(learn(&s, 1, 80, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, red));
  CHECK(learn(&s, 1, 81, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  req(&s, MXP_CMD_SET_FUNCTION, 2, &zero, 1);
  size_t n = mxs_peek_deferred(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 80);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) == n && seq == 80); /* not consumed by peek */
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 80 && r.payload[0] == 1);
  mxs_ack_deferred(&s);
  n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 81);
  CHECK(pop_deferred(&s, out, sizeof(out)) == 0);
  /* too small a buffer: 0, the packet stays queued */
  req(&s, MXP_CMD_SET_FUNCTION, 3, &one, 1);
  CHECK(learn(&s, 1, 82, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  CHECK(mxs_finish_learn(&s, seq, NULL));
  CHECK(!s.pending_learn.active);
  CHECK(mxs_peek_deferred(&s, &seq, out, 4) == 0);
  CHECK(mxs_peek_deferred(&s, &seq, out, sizeof(out)) > 0 && seq == 82);
}

/* ---- M8a: gesture (function 8) ---- */
static const uint8_t k_caps_gesture[4] = { 1, 2, 3, 8 };

static mxs_state_t *fresh_gesture(mxs_state_t *s)
{
  fresh(s);
  mxs_set_capabilities(s, k_caps_gesture, sizeof(k_caps_gesture));
  return s;
}

static const mxp_frame_t *get_fcfg(mxs_state_t *s, uint8_t fn)
{
  return req(s, MXP_CMD_GET_FUNC_CONFIG, 20, &fn, 1);
}

static const mxp_frame_t *set_fcfg(mxs_state_t *s, const uint8_t *p, uint16_t len)
{
  return req(s, MXP_CMD_SET_FUNC_CONFIG, 21, p, len);
}

static void test_get_info_nine_functions(void)
{
  mxs_state_t s;
  fresh(&s);
  const uint8_t nine[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  mxs_set_capabilities(&s, nine, 9);
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_INFO, 1, NULL, 0);
  CHECK(r->cmd == 0x81 && r->len == 23 + 9 && r->payload[22] == 9);
  for (int i = 0; i < 9; i++) CHECK(r->len == 32 && r->payload[23 + i] == i + 1);
  /* the whole GET_INFO list (MXP_INFO_MAX_FUNCS) fits too */
  uint8_t many[MXP_INFO_MAX_FUNCS];
  for (int i = 0; i < MXP_INFO_MAX_FUNCS; i++) many[i] = (uint8_t)(i + 1);
  mxs_set_capabilities(&s, many, MXP_INFO_MAX_FUNCS);
  r = req(&s, MXP_CMD_GET_INFO, 2, NULL, 0);
  CHECK(r->cmd == 0x81 && r->len == 23 + MXP_INFO_MAX_FUNCS && r->payload[22] == MXP_INFO_MAX_FUNCS);
}

static void test_func_config_gesture_get_default(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  const mxp_frame_t *r = get_fcfg(&s, 8);
  mxp_gesture_config_t def;
  mxp_gesture_config_defaults(&def);
  uint8_t want[MXP_FUNC_CONFIG_SIZE];
  mxp_pack_gesture_config(&def, want); /* 08 01 28 3c 01 00 00 00 00 00 today */
  CHECK(r->cmd == 0xC4 && r->seq == 20 && r->len == MXP_FUNC_CONFIG_SIZE &&
        memcmp(r->payload, want, sizeof(want)) == 0);
  CHECK(r->payload[0] == 8 && r->payload[1] == 1 && r->payload[3] == 60 && r->payload[4] == 1);
  for (int i = 5; i < MXP_FUNC_CONFIG_SIZE; i++) CHECK(r->payload[i] == 0);
  CHECK(g_data.cfg.dirty == 0);
}

static void test_func_config_gesture_set_clamps_and_dirty(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  uint32_t gen = s.data_gen;
  const uint8_t p[MXP_FUNC_CONFIG_SIZE] = { 8, 1, 5, 150, 0, 7, 7, 7, 7, 7 };
  const mxp_frame_t *r = set_fcfg(&s, p, sizeof(p));
  static const uint8_t want[MXP_FUNC_CONFIG_SIZE] = { 8, 1, 10, 100, 1, 0, 0, 0, 0, 0 };
  CHECK(r->cmd == 0xC5 && r->seq == 21 && r->len == MXP_FUNC_CONFIG_SIZE &&
        memcmp(r->payload, want, sizeof(want)) == 0);
  CHECK(g_data.gesture_cfg.hand_threshold == 10 && g_data.gesture_cfg.gesture_threshold == 100 &&
        g_data.gesture_cfg.max_hands == 1);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  /* GET_CONFIG reports dirty */
  CHECK(req(&s, MXP_CMD_GET_CONFIG, 22, NULL, 0)->payload[15] == 1);
  /* the same values again (clamping to the current ones): no change */
  g_data.cfg.dirty = 0;
  r = set_fcfg(&s, p, sizeof(p));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, want, sizeof(want)) == 0);
  CHECK(s.data_gen == gen + 1 && g_data.cfg.dirty == 0);
  r = set_fcfg(&s, want, sizeof(want));
  CHECK(r->cmd == 0xC5 && s.data_gen == gen + 1 && g_data.cfg.dirty == 0);
  /* GET returns the applied values */
  r = get_fcfg(&s, 8);
  CHECK(r->cmd == 0xC4 && memcmp(r->payload, want, sizeof(want)) == 0);
  /* a real change bumps again; works whatever the current function is */
  const uint8_t two[MXP_FUNC_CONFIG_SIZE] = { 8, 1, 30, 50, 2, 0, 0, 0, 0, 0 };
  CHECK(s.function_id == 0);
  r = set_fcfg(&s, two, sizeof(two));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, two, sizeof(two)) == 0);
  CHECK(s.data_gen == gen + 2 && g_data.cfg.dirty == 1 && g_data.gesture_cfg.max_hands == 2);
}

static void test_func_config_errors(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  uint32_t gen = s.data_gen;
  uint8_t two[2] = { 8, 0 };
  CHECK(is_err(req(&s, MXP_CMD_GET_FUNC_CONFIG, 1, NULL, 0), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(req(&s, MXP_CMD_GET_FUNC_CONFIG, 2, two, 2), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(get_fcfg(&s, 0), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 1), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 3), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 4), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM)); /* 4 not in GET_INFO here */
  CHECK(is_err(get_fcfg(&s, 200), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));

  uint8_t p[MXP_FUNC_CONFIG_SIZE + 1] = { 8, 1, 50, 70, 2, 0, 0, 0, 0, 0, 0 };
  CHECK(is_err(set_fcfg(&s, NULL, 0), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  uint8_t line[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 0, 0, 60, 8, 4, 80, 30, 0 };
  CHECK(is_err(set_fcfg(&s, line, sizeof(line)), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM)); /* not listed */
  uint8_t one = 1;
  CHECK(is_err(set_fcfg(&s, &one, 1), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, p, 1), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(set_fcfg(&s, p, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(set_fcfg(&s, p, 11), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  p[1] = 2;
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  p[1] = 0;
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  /* the length is checked before the layout version */
  CHECK(is_err(set_fcfg(&s, p, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(s.data_gen == gen && g_data.cfg.dirty == 0);
  mxp_gesture_config_t def;
  mxp_gesture_config_defaults(&def);
  CHECK(memcmp(&g_data.gesture_cfg, &def, sizeof(def)) == 0);

  /* gesture not in GET_INFO's list (model failed to load): BAD_PARAM both ways */
  fresh(&s); /* capabilities 1, 2 */
  p[1] = 1;
  CHECK(is_err(get_fcfg(&s, 8), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, p, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM)); /* BAD_PARAM before length */
  CHECK(g_data.cfg.dirty == 0);
}

static void test_gesture_names(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  mxst_set_name(&g_data, 2, 7, "door", 4);
  CHECK(set_name(&s, 8, 7, "\xE5\x89\x8D\xE9\x80\xB2", 6)->cmd == 0xB2 && g_r.len == 0); /* 前進 */
  uint8_t l = 0;
  const char *nm = mxst_get_name(&g_data, 8, 7, &l);
  CHECK(nm && l == 6 && memcmp(nm, "\xE5\x89\x8D\xE9\x80\xB2", 6) == 0 && g_data.cfg.dirty == 1);
  CHECK(is_err(set_name(&s, 8, 0, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 8, 10, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(set_name(&s, 8, 9, "down", 4)->cmd == 0xB2);
  CHECK(set_name(&s, 8, 1, "one", 3)->cmd == 0xB2);
  CHECK(g_data.n_names == 4);

  /* GET_LEARNED 8: named gestures by id, rgb 0 */
  uint8_t fn = 8;
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_LEARNED, 2, &fn, 1);
  static const uint8_t want[] = { 3, 1, 0, 0, 0, 0, 3, 'o', 'n', 'e',
                                  7, 0, 0, 0, 0, 6, 0xE5, 0x89, 0x8D, 0xE9, 0x80, 0xB2,
                                  9, 0, 0, 0, 0, 4, 'd', 'o', 'w', 'n' };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want) && memcmp(r->payload, want, sizeof(want)) == 0);

  /* FORGET 8: one name, absent names (success, not dirty), then all gesture names */
  g_data.cfg.dirty = 0;
  forget(&s, 8, 7, 3);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0 && mxst_get_name(&g_data, 8, 7, NULL) == NULL && g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 8, 3, 4);
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0 && g_data.n_names == 3);
  forget(&s, 8, 0xFFFF, 5);
  CHECK(g_r.cmd == 0xB1 && g_data.n_names == 1 && mxst_get_name(&g_data, 2, 7, NULL) != NULL &&
        g_data.cfg.dirty == 1);
  r = req(&s, MXP_CMD_GET_LEARNED, 6, &fn, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 && r->payload[0] == 0);
}

static void test_learn_gesture_rejected(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  uint8_t eight = 8, rb[64];
  mxp_frame_t r;
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 1, &eight, 1)->cmd == 0x90 && s.function_id == 8);
  CHECK(learn(&s, 8, 2, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && !s.pending_learn.active);
}

static void test_factory_reset_gesture_defaults(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  const uint8_t p[MXP_FUNC_CONFIG_SIZE] = { 8, 1, 30, 50, 2, 0, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, p, sizeof(p))->cmd == 0xC5);
  CHECK(set_name(&s, 8, 6, "go", 2)->cmd == 0xB2);
  uint8_t cmd = 0, seq = 0, rb[64];
  mxp_frame_t r;
  CHECK(deferred(&s, MXP_CMD_FACTORY_RESET, 30) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, NULL) && cmd == MXP_CMD_FACTORY_RESET);
  CHECK(mxs_complete_store(&s, 30, true));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC3);
  mxp_gesture_config_t def;
  mxp_gesture_config_defaults(&def);
  CHECK(memcmp(&g_data.gesture_cfg, &def, sizeof(def)) == 0 && g_data.n_names == 0);
  uint8_t want[MXP_FUNC_CONFIG_SIZE];
  mxp_pack_gesture_config(&def, want);
  const mxp_frame_t *g = get_fcfg(&s, 8);
  CHECK(g->cmd == 0xC4 && memcmp(g->payload, want, sizeof(want)) == 0);
}

static void test_save_snapshot_includes_gesture(void)
{
  mxs_state_t s;
  fresh_gesture(&s);
  static mxs_data_t snap;
  const uint8_t p[MXP_FUNC_CONFIG_SIZE] = { 8, 1, 55, 65, 2, 0, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, p, sizeof(p))->cmd == 0xC5);
  uint8_t cmd = 0, seq = 0;
  CHECK(deferred(&s, MXP_CMD_SAVE, 31) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap) && cmd == MXP_CMD_SAVE);
  CHECK(snap.gesture_cfg.hand_threshold == 55 && snap.gesture_cfg.gesture_threshold == 65 &&
        snap.gesture_cfg.max_hands == 2);
  CHECK(mxs_complete_store(&s, 31, true) && g_data.cfg.dirty == 0);
}

/* ---- M6: line tracking (4) and object tracking (5) ---- */
static const uint8_t k_caps_m6[5] = { 1, 2, 3, 4, 5 };

static mxs_state_t *fresh_m6(mxs_state_t *s)
{
  fresh(s);
  mxs_set_capabilities(s, k_caps_m6, sizeof(k_caps_m6));
  return s;
}

/* A valid object model without running the tracker. */
static void make_model(mxk_model_t *m, uint8_t tw, uint8_t th)
{
  memset(m, 0, sizeof(*m));
  m->tw = tw;
  m->th = th;
  m->box_w = (uint16_t)(tw * 2);
  m->box_h = (uint16_t)(th * 2);
  m->cx = 160;
  m->cy = 120;
  m->rgb[0] = 180;
  m->rgb[1] = 60;
  m->rgb[2] = 20;
  for (int i = 0; i < MXK_HIST_BINS; i++) m->hist[i] = (uint16_t)(i + 1);
  for (int i = 0; i < tw * th; i++) m->tmpl[i] = (uint8_t)(i * 13);
}

/* LEARN with an explicit payload length (9, or 11 with an id). */
static size_t learn_len(mxs_state_t *s, uint8_t func, uint8_t seq, uint16_t len, mxp_frame_t *r, uint8_t *rb,
                        size_t cap)
{
  uint8_t p[11], out[64];
  learn_payload(func, p);
  p[9] = 1;
  p[10] = 0;
  mxp_frame_t rq = { MXP_CMD_LEARN, seq, len, p };
  size_t n = mxs_handle(s, &rq, 0, out, sizeof(out));
  if (n != MXS_DEFERRED && n > 0) decode(out, n, r, rb, cap);
  return n;
}

static void test_get_info_lists_five(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_INFO, 1, NULL, 0);
  CHECK(r->cmd == 0x81 && r->len == 23 + 5 && r->payload[22] == 5);
  for (int i = 0; i < 5; i++) CHECK(r->payload[23 + i] == i + 1);
  uint8_t four = 4, five = 5;
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 2, &four, 1)->cmd == 0x90 && s.function_id == 4);
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 3, &five, 1)->cmd == 0x90 && s.function_id == 5);
}

static void test_func_config_line_track_default(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  static const uint8_t line[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 0, 0, 60, 8, 4, 80, 30, 0 };
  static const uint8_t track[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 50, 10, 1, 1, 0, 0, 0, 0 };
  const mxp_frame_t *r = get_fcfg(&s, 4);
  CHECK(r->cmd == 0xC4 && r->len == MXP_FUNC_CONFIG_SIZE && memcmp(r->payload, line, sizeof(line)) == 0);
  r = get_fcfg(&s, 5);
  CHECK(r->cmd == 0xC4 && r->len == MXP_FUNC_CONFIG_SIZE && memcmp(r->payload, track, sizeof(track)) == 0);
  CHECK(g_data.cfg.dirty == 0);
}

static void test_func_config_line_track_set(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint32_t gen = s.data_gen;
  /* line: clamped, reserved ignored; works while function 0 is current */
  const uint8_t lp[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 9, 100, 5, 30, 50, 10, 120, 7 };
  static const uint8_t lw[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 0, 100, 20, 12, 50, 50, 100, 0 };
  const mxp_frame_t *r = set_fcfg(&s, lp, sizeof(lp));
  CHECK(r->cmd == 0xC5 && r->len == MXP_FUNC_CONFIG_SIZE && memcmp(r->payload, lw, sizeof(lw)) == 0);
  CHECK(g_data.line_cfg.threshold == 100 && g_data.line_cfg.roi_pct == 20 && g_data.line_cfg.bands == 12 &&
        g_data.line_cfg.max_width == 50);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  g_data.cfg.dirty = 0;
  CHECK(set_fcfg(&s, lp, sizeof(lp))->cmd == 0xC5 && g_data.cfg.dirty == 0 && s.data_gen == gen + 1);
  CHECK(memcmp(get_fcfg(&s, 4)->payload, lw, sizeof(lw)) == 0);
  /* track */
  const uint8_t tp[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 5, 80, 3, 0, 1, 1, 1, 1 };
  static const uint8_t tw[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 10, 50, 1, 0, 0, 0, 0, 0 };
  r = set_fcfg(&s, tp, sizeof(tp));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, tw, sizeof(tw)) == 0);
  CHECK(g_data.track_cfg.threshold == 10 && g_data.track_cfg.adapt == 50 && g_data.track_cfg.reacquire == 1 &&
        g_data.track_cfg.scale == 0);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 2);
  g_data.cfg.dirty = 0;
  CHECK(set_fcfg(&s, tw, sizeof(tw))->cmd == 0xC5 && g_data.cfg.dirty == 0 && s.data_gen == gen + 2);
  /* the line config did not move */
  CHECK(memcmp(get_fcfg(&s, 4)->payload, lw, sizeof(lw)) == 0);
}

static void test_func_config_line_track_errors(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t p[MXP_FUNC_CONFIG_SIZE + 1] = { 4, 1, 0, 0, 60, 8, 4, 80, 30, 0, 0 };
  CHECK(is_err(set_fcfg(&s, p, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(set_fcfg(&s, p, 11), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  p[1] = 2;
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  p[0] = 5;
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  /* functions without a func config, and 8 when it is not in GET_INFO */
  CHECK(is_err(get_fcfg(&s, 1), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 3), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 8), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(g_data.cfg.dirty == 0);
  /* 4 / 5 not in GET_INFO's list: BAD_PARAM (protocol.md 9), before the length */
  fresh(&s);
  p[0] = 4;
  p[1] = 1;
  CHECK(is_err(get_fcfg(&s, 4), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 5), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, p, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, p, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(g_data.cfg.dirty == 0);
}

static void test_learn_with_id_rejected(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t rb[64];
  mxp_frame_t r;
  for (uint8_t fn = 1; fn <= 5; fn++) {
    if (fn == 2 || fn == 3) continue;
    CHECK(req(&s, MXP_CMD_SET_FUNCTION, 1, &fn, 1)->cmd == 0x90 && s.function_id == fn);
    CHECK(learn_len(&s, fn, 40, 11, &r, rb, sizeof(rb)) != MXS_DEFERRED);
    CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 40 && !s.pending_learn.active);
    /* the 9-byte form is still accepted */
    CHECK(learn_len(&s, fn, 41, 9, &r, rb, sizeof(rb)) == MXS_DEFERRED && s.pending_learn.active);
    /* ...and an 11-byte one is BAD_PARAM, not BUSY, while it is pending */
    CHECK(learn_len(&s, fn, 42, 11, &r, rb, sizeof(rb)) != MXS_DEFERRED);
    CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
    uint8_t zero = 0;
    req(&s, MXP_CMD_SET_FUNCTION, 2, &zero, 1); /* cancels the pending one */
    uint8_t out[16];
    while (pop_deferred(&s, out, sizeof(out)) > 0) {}
  }
  /* other lengths stay BAD_LENGTH */
  uint8_t one = 1;
  req(&s, MXP_CMD_SET_FUNCTION, 3, &one, 1);
  CHECK(learn_len(&s, 1, 43, 10, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_LENGTH));
}

static void test_learn_fn_mismatch(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t rb[64], four = 4, five = 5;
  mxp_frame_t r;
  req(&s, MXP_CMD_SET_FUNCTION, 1, &four, 1);
  CHECK(learn(&s, 5, 2, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
  CHECK(learn(&s, 1, 3, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
  req(&s, MXP_CMD_SET_FUNCTION, 4, &five, 1);
  CHECK(learn(&s, 4, 5, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
  CHECK(!s.pending_learn.active);
  /* function 0 current: nothing can be learned */
  uint8_t zero = 0;
  req(&s, MXP_CMD_SET_FUNCTION, 6, &zero, 1);
  CHECK(learn(&s, 4, 7, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
}

static void test_learn_line_flow(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t rb[64], out[64], four = 4, seq = 0;
  mxp_frame_t r;
  mxp_learn_req_t lr;
  const uint8_t teal[3] = { 20, 200, 180 }, orange[3] = { 240, 120, 20 };
  req(&s, MXP_CMD_SET_FUNCTION, 1, &four, 1);
  uint32_t gen = s.data_gen;
  CHECK(learn(&s, 4, 50, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(learn(&s, 4, 51, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BUSY));
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && seq == 50 && lr.function_id == 4);
  CHECK(mxs_finish_learn(&s, seq, teal));
  size_t n = pop_deferred(&s, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 50 && r.len == 1 && r.payload[0] == 1);
  CHECK(g_data.line_has_color == 1 && memcmp(g_data.line_rgb, teal, 3) == 0);
  CHECK(g_data.line_cfg.mode == MXP_LINE_MODE_LEARNED && g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  CHECK(g_data.n_colors == 0); /* the colour function's list is untouched */
  CHECK(get_fcfg(&s, 4)->payload[2] == 2);
  /* the same colour again: id 1, nothing changed */
  g_data.cfg.dirty = 0;
  CHECK(learn(&s, 4, 52, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, teal));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.payload[0] == 1 && g_data.cfg.dirty == 0 && s.data_gen == gen + 1);
  /* another colour replaces it */
  CHECK(learn(&s, 4, 53, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, orange));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.payload[0] == 1 && memcmp(g_data.line_rgb, orange, 3) == 0 && g_data.cfg.dirty == 1);
  /* same colour, but the mode was changed to 0 meanwhile: learning sets 2 again (dirty) */
  g_data.line_cfg.mode = 0;
  g_data.cfg.dirty = 0;
  CHECK(learn(&s, 4, 54, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, orange));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && g_data.line_cfg.mode == 2 && g_data.cfg.dirty == 1);
  /* cancelled meanwhile (function change): nothing learned */
  CHECK(learn(&s, 4, 55, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  uint8_t zero = 0;
  req(&s, MXP_CMD_SET_FUNCTION, 2, &zero, 1);
  CHECK(!mxs_finish_learn(&s, seq, teal) && memcmp(g_data.line_rgb, orange, 3) == 0);
}

static void test_learn_line_no_color(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t rb[64], out[64], four = 4, seq = 0;
  mxp_frame_t r;
  mxp_learn_req_t lr;
  const uint8_t grey[3] = { 90, 90, 90 };
  req(&s, MXP_CMD_SET_FUNCTION, 1, &four, 1);
  /* region without a usable colour (NULL) or a grey one (black / white tape) */
  CHECK(learn(&s, 4, 60, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, NULL));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 60);
  CHECK(learn(&s, 4, 61, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, grey));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 61);
  CHECK(g_data.line_has_color == 0 && g_data.line_cfg.mode == 0 && g_data.cfg.dirty == 0);
  CHECK(!s.pending_learn.active);
}

static void test_learn_object_flow(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t rb[64], out[64], five = 5, seq = 0;
  mxp_frame_t r;
  mxp_learn_req_t lr;
  mxk_model_t m, m2;
  make_model(&m, 20, 20);
  make_model(&m2, 32, 16);
  req(&s, MXP_CMD_SET_FUNCTION, 1, &five, 1);
  uint32_t gen = s.data_gen;
  CHECK(learn(&s, 5, 70, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(!mxs_finish_learn_object(&s, 70, &m)); /* not claimed yet */
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && lr.function_id == 5 && lr.x == 100 && lr.w == 20);
  CHECK(mxs_finish_learn_object(&s, seq, &m));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 70 && r.len == 1 && r.payload[0] == 1);
  CHECK(g_data.has_object == 1 && g_data.object.tw == 20 && g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  /* name it, then learn again: replaced and the name is gone */
  CHECK(set_name(&s, 5, 1, "cup", 3)->cmd == 0xB2);
  CHECK(learn(&s, 5, 71, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn_object(&s, seq, &m2));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.payload[0] == 1 && g_data.object.tw == 32 && g_data.object.th == 16);
  CHECK(mxst_get_name(&g_data, 5, 1, NULL) == NULL);
  /* nothing to track (too small / flat region: NULL) or an invalid model -> BAD_PARAM, object kept */
  g_data.cfg.dirty = 0;
  CHECK(learn(&s, 5, 72, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn_object(&s, seq, NULL));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 72);
  mxk_model_t bad = m;
  bad.tw = 40;
  CHECK(learn(&s, 5, 73, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn_object(&s, seq, &bad));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 73);
  CHECK(g_data.has_object == 1 && g_data.object.tw == 32 && g_data.cfg.dirty == 0);
  /* the colour completion on an object learn (and vice versa) is a BAD_PARAM, not a crash */
  CHECK(learn(&s, 5, 74, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  const uint8_t red[3] = { 230, 20, 20 };
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, red));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && g_data.n_colors == 0 && g_data.line_has_color == 0);
  uint8_t one = 1;
  req(&s, MXP_CMD_SET_FUNCTION, 2, &one, 1);
  CHECK(learn(&s, 1, 75, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn_object(&s, seq, &m));
  decode(out, pop_deferred(&s, out, sizeof(out)), &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && g_data.object.tw == 32);
  /* cancelled meanwhile: nothing learned, false */
  req(&s, MXP_CMD_SET_FUNCTION, 3, &five, 1);
  CHECK(learn(&s, 5, 76, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  req(&s, MXP_CMD_SET_FUNCTION, 4, &one, 1);
  CHECK(!mxs_finish_learn_object(&s, seq, &m) && g_data.object.tw == 32);
}

static void test_forget_line_resets_mode(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  const uint8_t teal[3] = { 20, 200, 180 };
  CHECK(mxst_set_line_color(&g_data, teal));
  /* other ids: nothing to forget */
  forget(&s, 4, 2, 1);
  CHECK(g_r.cmd == 0xB1 && g_data.line_has_color == 1 && g_data.cfg.dirty == 0);
  forget(&s, 4, 1, 2);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0 && g_data.line_has_color == 0 && g_data.line_cfg.mode == 0);
  CHECK(g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 4, 1, 3); /* again: idempotent, not dirty */
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  /* 0xFFFF works too; mode 1 is kept */
  CHECK(mxst_set_line_color(&g_data, teal));
  g_data.line_cfg.mode = 1;
  forget(&s, 4, 0xFFFF, 4);
  CHECK(g_r.cmd == 0xB1 && g_data.line_has_color == 0 && g_data.line_cfg.mode == 1 && g_data.cfg.dirty == 1);
  /* mode 2 set by SET_FUNC_CONFIG without a colour goes back to 0 (dirty) */
  g_data.cfg.dirty = 0;
  g_data.line_cfg.mode = 2;
  forget(&s, 4, 1, 5);
  CHECK(g_r.cmd == 0xB1 && g_data.line_cfg.mode == 0 && g_data.cfg.dirty == 1);
  /* FORGET 4 does not depend on GET_INFO's list, like the other functions */
  fresh(&s);
  forget(&s, 4, 1, 6);
  CHECK(g_r.cmd == 0xB1);
}

static void test_forget_object_and_name(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  mxk_model_t m;
  make_model(&m, 16, 16);
  CHECK(mxst_set_object(&g_data, &m));
  CHECK(mxst_set_name(&g_data, 5, 1, "ball", 4));
  CHECK(mxst_set_name(&g_data, 2, 1, "tag", 3));
  forget(&s, 5, 3, 1);
  CHECK(g_r.cmd == 0xB1 && g_data.has_object == 1 && g_data.cfg.dirty == 0);
  forget(&s, 5, 1, 2);
  CHECK(g_r.cmd == 0xB1 && g_data.has_object == 0 && mxst_get_name(&g_data, 5, 1, NULL) == NULL);
  CHECK(mxst_get_name(&g_data, 2, 1, NULL) != NULL && g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 5, 0xFFFF, 3);
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  CHECK(mxst_set_object(&g_data, &m));
  forget(&s, 5, 0xFFFF, 4);
  CHECK(g_r.cmd == 0xB1 && g_data.has_object == 0 && g_data.cfg.dirty == 1);
  /* unknown functions are still BAD_PARAM (6 is QR since M7) */
  forget(&s, 7, 1, 5);
  CHECK(is_err(&g_r, MXP_CMD_FORGET, MXP_ERR_BAD_PARAM));
}

static void test_set_name_rules_m6(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  /* line: never */
  CHECK(is_err(set_name(&s, 4, 1, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 4, 1, "", 0), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  /* object: only id 1, only once learned */
  CHECK(is_err(set_name(&s, 5, 1, "cup", 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  mxk_model_t m;
  make_model(&m, 16, 16);
  CHECK(mxst_set_object(&g_data, &m));
  CHECK(is_err(set_name(&s, 5, 0, "cup", 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 5, 2, "cup", 3), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(g_data.n_names == 0 && g_data.cfg.dirty == 0);
  CHECK(set_name(&s, 5, 1, "\xE6\x9D\xAF", 3)->cmd == 0xB2 && g_r.len == 0); /* 杯 */
  uint8_t l = 0;
  const char *nm = mxst_get_name(&g_data, 5, 1, &l);
  CHECK(nm && l == 3 && memcmp(nm, "\xE6\x9D\xAF", 3) == 0 && g_data.cfg.dirty == 1);
  /* clearing works; the shared 32-name table applies */
  CHECK(set_name(&s, 5, 1, "", 0)->cmd == 0xB2 && g_data.n_names == 0);
  for (uint16_t i = 0; i < MXST_MAX_NAMES; i++) CHECK(set_name(&s, 2, i, "t", 1)->cmd == 0xB2);
  CHECK(is_err(set_name(&s, 5, 1, "cup", 3), MXP_CMD_SET_NAME, MXP_ERR_FULL));
  /* face still has no names */
  CHECK(is_err(set_name(&s, 3, 0, "me", 2), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
}

static void test_get_learned_line_and_object(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  uint8_t four = 4, five = 5;
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_LEARNED, 1, &four, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 && r->payload[0] == 0);
  r = req(&s, MXP_CMD_GET_LEARNED, 2, &five, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 && r->payload[0] == 0);
  const uint8_t teal[3] = { 20, 200, 180 };
  CHECK(mxst_set_line_color(&g_data, teal));
  r = req(&s, MXP_CMD_GET_LEARNED, 3, &four, 1);
  static const uint8_t want4[] = { 1, 1, 0, 20, 200, 180, 0 };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want4) && memcmp(r->payload, want4, sizeof(want4)) == 0);
  mxk_model_t m;
  make_model(&m, 16, 16);
  CHECK(mxst_set_object(&g_data, &m));
  r = req(&s, MXP_CMD_GET_LEARNED, 4, &five, 1);
  static const uint8_t want5a[] = { 1, 1, 0, 180, 60, 20, 0 };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want5a) && memcmp(r->payload, want5a, sizeof(want5a)) == 0);
  CHECK(mxst_set_name(&g_data, 5, 1, "ball", 4));
  r = req(&s, MXP_CMD_GET_LEARNED, 5, &five, 1);
  static const uint8_t want5b[] = { 1, 1, 0, 180, 60, 20, 4, 'b', 'a', 'l', 'l' };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want5b) && memcmp(r->payload, want5b, sizeof(want5b)) == 0);
  /* face still BAD_PARAM */
  uint8_t three = 3;
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED, 6, &three, 1), MXP_CMD_GET_LEARNED, MXP_ERR_BAD_PARAM));
}

static void test_factory_reset_m6(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  const uint8_t lp[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 1, 0, 40, 6, 4, 80, 30, 0 };
  const uint8_t tp[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 70, 0, 0, 0, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, lp, sizeof(lp))->cmd == 0xC5 && set_fcfg(&s, tp, sizeof(tp))->cmd == 0xC5);
  const uint8_t teal[3] = { 20, 200, 180 };
  CHECK(mxst_set_line_color(&g_data, teal));
  mxk_model_t m;
  make_model(&m, 16, 16);
  CHECK(mxst_set_object(&g_data, &m) && mxst_set_name(&g_data, 5, 1, "ball", 4));
  uint8_t cmd = 0, seq = 0, rb[64];
  mxp_frame_t r;
  CHECK(deferred(&s, MXP_CMD_FACTORY_RESET, 30) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, NULL) && mxs_complete_store(&s, 30, true));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC3);
  static const uint8_t line[MXP_FUNC_CONFIG_SIZE] = { 4, 1, 0, 0, 60, 8, 4, 80, 30, 0 };
  static const uint8_t track[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 50, 10, 1, 1, 0, 0, 0, 0 };
  CHECK(memcmp(get_fcfg(&s, 4)->payload, line, sizeof(line)) == 0);
  CHECK(memcmp(get_fcfg(&s, 5)->payload, track, sizeof(track)) == 0);
  CHECK(g_data.line_has_color == 0 && g_data.has_object == 0 && g_data.n_names == 0 && g_data.cfg.dirty == 0);
}

static void test_save_snapshot_includes_object(void)
{
  mxs_state_t s;
  fresh_m6(&s);
  static mxs_data_t snap;
  const uint8_t teal[3] = { 20, 200, 180 };
  mxk_model_t m;
  make_model(&m, 24, 18);
  CHECK(mxst_set_line_color(&g_data, teal) && mxst_set_object(&g_data, &m));
  const uint8_t tp[MXP_FUNC_CONFIG_SIZE] = { 5, 1, 70, 20, 0, 1, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, tp, sizeof(tp))->cmd == 0xC5);
  uint8_t cmd = 0, seq = 0;
  CHECK(deferred(&s, MXP_CMD_SAVE, 31) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap) && cmd == MXP_CMD_SAVE);
  CHECK(snap.has_object == 1 && memcmp(&snap.object, &m, sizeof(m)) == 0);
  CHECK(snap.line_has_color == 1 && memcmp(snap.line_rgb, teal, 3) == 0 && snap.line_cfg.mode == 2);
  CHECK(snap.track_cfg.threshold == 70 && snap.track_cfg.adapt == 20 && snap.track_cfg.reacquire == 0);
  /* the snapshot serialises */
  static uint8_t blob[MXST_BLOB_MAX];
  CHECK(mxst_serialize(&snap, blob, sizeof(blob)) > 0);
  CHECK(mxs_complete_store(&s, 31, true) && g_data.cfg.dirty == 0);
}

/* ---- M7: QR (6), shapes (11), motion (12) ---- */
static const uint8_t k_caps_m7[9] = { 1, 2, 3, 4, 5, 6, 8, 11, 12 };

static mxs_state_t *fresh_m7(mxs_state_t *s)
{
  fresh(s);
  mxs_set_capabilities(s, k_caps_m7, sizeof(k_caps_m7));
  return s;
}

/* Decodes a hex string (test vectors) into out; returns the byte count. */
static size_t unhex(const char *h, uint8_t *out)
{
  size_t n = 0;
  for (; h[0] && h[1]; h += 2) {
    unsigned v = 0;
    for (int k = 0; k < 2; k++) {
      char c = h[k];
      v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out[n++] = (uint8_t)v;
  }
  return n;
}

static bool payload_is_hex(const mxp_frame_t *r, const char *hex)
{
  uint8_t want[300];
  size_t n = unhex(hex, want);
  return r->len == n && memcmp(r->payload, want, n) == 0;
}

/* LEARN 6 (current function 6) completed with mxs_finish_learn_qr; the
 * deferred reply is decoded into *r. Returns mxs_finish_learn_qr's result. */
static bool learn_qr(mxs_state_t *s, uint8_t seq, const char *bytes, uint8_t len, uint16_t total, mxp_frame_t *r,
                     uint8_t *rb, size_t cap)
{
  uint8_t out[64], got = 0;
  mxp_learn_req_t lr;
  CHECK(learn(s, 6, seq, 0, r, rb, cap) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(s, &got, &lr) && got == seq && lr.function_id == 6);
  bool ok = mxs_finish_learn_qr(s, got, (const uint8_t *)bytes, len, total);
  size_t n = pop_deferred(s, out, sizeof(out));
  CHECK(n > 0);
  if (n > 0) decode(out, n, r, rb, cap);
  return ok;
}

static void test_get_info_lists_m7(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_INFO, 1, NULL, 0);
  CHECK(r->cmd == 0x81 && r->len == 23 + 9 && r->payload[22] == 9);
  CHECK(r->len == 32 && memcmp(r->payload + 23, k_caps_m7, sizeof(k_caps_m7)) == 0);
  uint8_t six = 6, eleven = 11, twelve = 12;
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 2, &six, 1)->cmd == 0x90 && s.function_id == 6);
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 3, &eleven, 1)->cmd == 0x90 && s.function_id == 11);
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 4, &twelve, 1)->cmd == 0x90 && s.function_id == 12);
  /* without them in the list (e.g. M6 capabilities): BAD_PARAM */
  fresh_m6(&s);
  CHECK(is_err(req(&s, MXP_CMD_SET_FUNCTION, 5, &six, 1), MXP_CMD_SET_FUNCTION, MXP_ERR_BAD_PARAM));
}

static void test_func_config_shape_motion(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint32_t gen = s.data_gen;
  /* defaults = the Task 1 vectors func_config_shape_default / func_config_motion_default */
  const mxp_frame_t *r = get_fcfg(&s, 11);
  CHECK(r->cmd == 0xC4 && payload_is_hex(r, "0b010000200400000000"));
  r = get_fcfg(&s, 12);
  CHECK(r->cmd == 0xC4 && payload_is_hex(r, "0c011918040000000000"));
  CHECK(g_data.cfg.dirty == 0);
  /* QR has no func config */
  CHECK(is_err(get_fcfg(&s, 6), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  /* SET 12 (vector set_func_config_motion_request): applied as is, dirty */
  uint8_t mp[MXP_FUNC_CONFIG_SIZE];
  unhex("0c010f30020000000000", mp);
  r = set_fcfg(&s, mp, sizeof(mp));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, mp, sizeof(mp)) == 0);
  CHECK(g_data.motion_cfg.threshold == 15 && g_data.motion_cfg.min_size == 48 && g_data.motion_cfg.max_results == 2);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  /* clamped, reserved ignored */
  const uint8_t mw[MXP_FUNC_CONFIG_SIZE] = { 12, 1, 1, 250, 0, 9, 9, 9, 9, 9 };
  static const uint8_t mwant[MXP_FUNC_CONFIG_SIZE] = { 12, 1, 5, 240, 1, 0, 0, 0, 0, 0 };
  r = set_fcfg(&s, mw, sizeof(mw));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, mwant, sizeof(mwant)) == 0 && s.data_gen == gen + 2);
  /* the same values again: not dirty */
  g_data.cfg.dirty = 0;
  CHECK(set_fcfg(&s, mw, sizeof(mw))->cmd == 0xC5 && g_data.cfg.dirty == 0 && s.data_gen == gen + 2);
  CHECK(memcmp(get_fcfg(&s, 12)->payload, mwant, sizeof(mwant)) == 0);
  /* shape: clamped, whatever the current function */
  const uint8_t sp[MXP_FUNC_CONFIG_SIZE] = { 11, 1, 3, 200, 5, 20, 9, 9, 9, 9 };
  static const uint8_t swant[MXP_FUNC_CONFIG_SIZE] = { 11, 1, 0, 200, 16, 8, 0, 0, 0, 0 };
  r = set_fcfg(&s, sp, sizeof(sp));
  CHECK(r->cmd == 0xC5 && memcmp(r->payload, swant, sizeof(swant)) == 0);
  CHECK(g_data.shape_cfg.threshold == 200 && g_data.shape_cfg.min_size == 16 && g_data.shape_cfg.max_results == 8);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 3);
  /* errors: wrong layout version, wrong length */
  uint8_t bad[MXP_FUNC_CONFIG_SIZE + 1] = { 11, 2, 0, 0, 32, 4, 0, 0, 0, 0, 0 };
  CHECK(is_err(set_fcfg(&s, bad, 10), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  bad[1] = 1;
  CHECK(is_err(set_fcfg(&s, bad, 11), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(set_fcfg(&s, bad, 9), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_LENGTH));
  CHECK(s.data_gen == gen + 3);
  /* 11 / 12 not in GET_INFO's list (protocol.md 9): BAD_PARAM */
  fresh_m6(&s);
  CHECK(is_err(get_fcfg(&s, 11), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_fcfg(&s, 12), MXP_CMD_GET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_fcfg(&s, mp, sizeof(mp)), MXP_CMD_SET_FUNC_CONFIG, MXP_ERR_BAD_PARAM));
  CHECK(g_data.cfg.dirty == 0);
}

static void test_learn_qr_flow(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t rb[64], six = 6, zero = 0, seq = 0;
  mxp_frame_t r;
  mxp_learn_req_t lr;
  req(&s, MXP_CMD_SET_FUNCTION, 1, &six, 1);
  uint32_t gen = s.data_gen;
  /* a new code: id 1, dirty */
  CHECK(learn_qr(&s, 90, "LEFT", 4, 4, &r, rb, sizeof(rb)));
  CHECK(r.cmd == 0xB0 && r.seq == 90 && r.len == 1 && r.payload[0] == 1);
  CHECK(g_data.n_qr == 1 && g_data.qr[0].id == 1 && memcmp(g_data.qr[0].bytes, "LEFT", 4) == 0);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  /* the same content again: id 1, nothing changed */
  g_data.cfg.dirty = 0;
  CHECK(learn_qr(&s, 91, "LEFT", 4, 4, &r, rb, sizeof(rb)));
  CHECK(r.cmd == 0xB0 && r.seq == 91 && r.payload[0] == 1);
  CHECK(g_data.n_qr == 1 && g_data.cfg.dirty == 0 && s.data_gen == gen + 1);
  CHECK(learn_qr(&s, 92, "RIGHT", 5, 5, &r, rb, sizeof(rb)));
  CHECK(r.cmd == 0xB0 && r.payload[0] == 2 && g_data.cfg.dirty == 1 && s.data_gen == gen + 2);
  /* no decoded code in the region (NULL) or an empty one -> BAD_PARAM, nothing changed */
  g_data.cfg.dirty = 0;
  CHECK(learn_qr(&s, 93, NULL, 0, 0, &r, rb, sizeof(rb)));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 93);
  CHECK(learn_qr(&s, 94, "", 0, 0, &r, rb, sizeof(rb)));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 94);
  CHECK(g_data.n_qr == 2 && g_data.cfg.dirty == 0 && s.data_gen == gen + 2 && !s.pending_learn.active);
  /* full -> FULL; a learned code still answers its id */
  for (int i = 3; i <= MXST_MAX_QR; i++) {
    char c[2] = { (char)('a' + i), 0 };
    CHECK(learn_qr(&s, (uint8_t)(100 + i), c, 1, 1, &r, rb, sizeof(rb)) && r.cmd == 0xB0 && r.payload[0] == i);
  }
  CHECK(learn_qr(&s, 110, "NEW", 3, 3, &r, rb, sizeof(rb)));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_FULL) && r.seq == 110 && g_data.n_qr == MXST_MAX_QR);
  CHECK(learn_qr(&s, 111, "RIGHT", 5, 5, &r, rb, sizeof(rb)) && r.cmd == 0xB0 && r.payload[0] == 2);
  /* not claimed yet: false, nothing queued */
  CHECK(learn(&s, 6, 112, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(!mxs_finish_learn_qr(&s, 112, (const uint8_t *)"X", 1, 1));
  CHECK(s.outbox_count == 0);
  /* the colour / object completions on a QR learn are a BAD_PARAM, not a learn */
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn(&s, seq, (const uint8_t *)"\xE6\x14\x14"));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && g_data.n_colors == 0);
  /* and the QR completion on a colour learn too */
  uint8_t one = 1;
  req(&s, MXP_CMD_SET_FUNCTION, 2, &one, 1);
  CHECK(learn(&s, 1, 113, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr) && mxs_finish_learn_qr(&s, seq, (const uint8_t *)"Z", 1, 1));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && g_data.n_qr == MXST_MAX_QR);
  /* cancelled meanwhile (function change): false, nothing learned */
  mxst_forget_qr(&g_data, 0xFFFF);
  req(&s, MXP_CMD_SET_FUNCTION, 3, &six, 1);
  CHECK(learn(&s, 6, 114, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &lr));
  req(&s, MXP_CMD_SET_FUNCTION, 4, &zero, 1);
  CHECK(!mxs_finish_learn_qr(&s, seq, (const uint8_t *)"X", 1, 1) && g_data.n_qr == 0);
}

static void test_learn_shape_motion_rejected(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t rb[64], eleven = 11, twelve = 12;
  mxp_frame_t r;
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 1, &eleven, 1)->cmd == 0x90);
  CHECK(learn(&s, 11, 2, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && !s.pending_learn.active);
  CHECK(req(&s, MXP_CMD_SET_FUNCTION, 3, &twelve, 1)->cmd == 0x90);
  CHECK(learn(&s, 12, 4, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && !s.pending_learn.active);
  /* LEARN 6 while 12 is current: BAD_PARAM too */
  CHECK(learn(&s, 6, 5, 0, &r, rb, sizeof(rb)) != MXS_DEFERRED && is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM));
}

static void test_learn_11_bytes_still_rejected(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t rb[64], six = 6;
  mxp_frame_t r;
  req(&s, MXP_CMD_SET_FUNCTION, 1, &six, 1);
  CHECK(learn_len(&s, 6, 40, 11, &r, rb, sizeof(rb)) != MXS_DEFERRED);
  CHECK(is_err(&r, MXP_CMD_LEARN, MXP_ERR_BAD_PARAM) && r.seq == 40 && !s.pending_learn.active);
  CHECK(learn_len(&s, 6, 41, 9, &r, rb, sizeof(rb)) == MXS_DEFERRED && s.pending_learn.active);
}

static mxs_rdata_t rdata(const char *bytes, uint8_t len, uint16_t total, uint8_t flags)
{
  mxs_rdata_t d;
  memset(&d, 0, sizeof(d));
  d.flags = flags;
  d.total_len = total;
  d.len = len;
  if (len) memcpy(d.data, bytes, len);
  return d;
}

/* Caches a batch of `count` QR records (frame_id `frame`) as the pipeline does. */
static void set_batch(mxs_state_t *s, uint32_t frame, uint8_t fn, uint8_t count)
{
  mxp_results_t in;
  memset(&in, 0, sizeof(in));
  in.frame_id = frame;
  in.function_id = fn;
  in.count = count;
  for (int i = 0; i < count; i++) {
    in.r[i].x = (int16_t)(50 + 40 * i);
    in.r[i].y = 100;
    in.r[i].w = 30;
    in.r[i].h = 30;
    in.r[i].confidence = 255;
  }
  uint8_t packed[MXS_RESULTS_MAX];
  size_t n = mxp_pack_results(&in, packed, sizeof(packed));
  mxs_set_results(s, packed, (uint16_t)n);
}

static const mxp_frame_t *get_rdata(mxs_state_t *s, uint8_t index)
{
  return req(s, MXP_CMD_GET_RESULT_DATA, 17, &index, 1);
}

static void test_result_data(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t six = 6, eleven = 11, two[2] = { 0, 0 };
  req(&s, MXP_CMD_SET_FUNCTION, 1, &six, 1);
  /* no processed frame yet: BAD_PARAM; lengths other than 1: BAD_LENGTH */
  CHECK(is_err(get_rdata(&s, 0), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_PARAM));
  CHECK(is_err(req(&s, MXP_CMD_GET_RESULT_DATA, 2, NULL, 0), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(req(&s, MXP_CMD_GET_RESULT_DATA, 3, two, 2), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_LENGTH));
  /* 2 records, data for the first: the vector result_data_hello */
  set_batch(&s, 300, 6, 2);
  mxs_rdata_t items[3];
  items[0] = rdata("HELLO", 5, 5, 0);
  mxs_set_result_data(&s, items, 1);
  const mxp_frame_t *r = get_rdata(&s, 0);
  CHECK(r->cmd == 0xA1 && r->seq == 17 && payload_is_hex(r, "2c010000000005000548454c4c4f"));
  /* the second has none: len 0 with the batch's frame_id */
  r = get_rdata(&s, 1);
  mxp_result_data_t d;
  CHECK(r->cmd == 0xA1 && mxp_unpack_result_data(r->payload, r->len, &d) && d.frame_id == 300 && d.index == 1 &&
        d.flags == 0 && d.total_len == 0 && d.len == 0);
  CHECK(is_err(get_rdata(&s, 2), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_rdata(&s, 255), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_PARAM));
  /* long content (vector result_data_truncated: index 1, total 300, first 128 bytes) */
  set_batch(&s, 301, 6, 2);
  uint8_t big[MXP_RESULT_DATA_MAX];
  for (int i = 0; i < MXP_RESULT_DATA_MAX; i++) big[i] = (uint8_t)i;
  items[0] = rdata("A", 1, 1, 0);
  items[1] = rdata((const char *)big, MXP_RESULT_DATA_MAX, 300, MXP_DATA_FLAG_TRUNCATED);
  mxs_set_result_data(&s, items, 2);
  r = get_rdata(&s, 1);
  CHECK(r->cmd == 0xA1 && r->len == 9 + 128);
  CHECK(payload_is_hex(r, "2d01000001012c0180000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223"
                          "2425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152"
                          "535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"));
  /* the next batch without data: len 0 (old content is not carried over) */
  set_batch(&s, 302, 6, 2);
  r = get_rdata(&s, 0);
  CHECK(r->cmd == 0xA1 && mxp_unpack_result_data(r->payload, r->len, &d) && d.frame_id == 302 && d.len == 0);
  /* more items than records: only the batch's records get data */
  set_batch(&s, 303, 6, 1);
  items[0] = rdata("ONE", 3, 3, 0);
  items[1] = rdata("TWO", 3, 3, 0);
  items[2] = rdata("SIX", 3, 3, 0);
  mxs_set_result_data(&s, items, 3);
  r = get_rdata(&s, 0);
  CHECK(r->cmd == 0xA1 && mxp_unpack_result_data(r->payload, r->len, &d) && d.len == 3 &&
        memcmp(d.data, "ONE", 3) == 0 && d.frame_id == 303);
  CHECK(is_err(get_rdata(&s, 1), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_PARAM));
  /* an ignored (oversized) batch keeps the previous batch and its data */
  uint8_t junk[MXS_RESULTS_MAX + 1];
  memset(junk, 0, sizeof(junk));
  mxs_set_results(&s, junk, MXS_RESULTS_MAX + 1);
  r = get_rdata(&s, 0);
  CHECK(r->cmd == 0xA1 && mxp_unpack_result_data(r->payload, r->len, &d) && d.len == 3 && d.frame_id == 303);
  /* a function change empties the cache: BAD_PARAM */
  req(&s, MXP_CMD_SET_FUNCTION, 4, &eleven, 1);
  CHECK(is_err(get_rdata(&s, 0), MXP_CMD_GET_RESULT_DATA, MXP_ERR_BAD_PARAM));
  /* other functions' records have no data: len 0 */
  set_batch(&s, 304, 11, 1);
  r = get_rdata(&s, 0);
  CHECK(r->cmd == 0xA1 && mxp_unpack_result_data(r->payload, r->len, &d) && d.len == 0 && d.total_len == 0);
}

static const mxp_frame_t *get_ldata(mxs_state_t *s, uint8_t fn, uint16_t id)
{
  uint8_t p[3] = { fn, (uint8_t)id, (uint8_t)(id >> 8) };
  return req(s, MXP_CMD_GET_LEARNED_DATA, 19, p, 3);
}

static void test_learned_data(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"LEFT", 4, 4) == 1);
  const mxp_frame_t *r = get_ldata(&s, 6, 1);
  mxp_learned_data_t d;
  CHECK(r->cmd == 0xB5 && r->seq == 19 && mxp_unpack_learned_data(r->payload, r->len, &d));
  CHECK(d.function_id == 6 && d.id == 1 && d.flags == 0 && d.total_len == 4 && d.len == 4 &&
        memcmp(d.data, "LEFT", 4) == 0);
  /* not learned, other functions: BAD_PARAM; lengths other than 3: BAD_LENGTH */
  CHECK(is_err(get_ldata(&s, 6, 2), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_ldata(&s, 6, 0), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_ldata(&s, 1, 1), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
  CHECK(is_err(get_ldata(&s, 11, 1), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
  uint8_t p[4] = { 6, 1, 0, 0 };
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED_DATA, 2, p, 2), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_LENGTH));
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED_DATA, 3, p, 4), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_LENGTH));
  /* the vector learned_data_qr: id 2 = "LEFT" */
  mxst_forget_qr(&g_data, 0xFFFF);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"X", 1, 1) == 1);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"LEFT", 4, 4) == 2);
  r = get_ldata(&s, 6, 2);
  CHECK(r->cmd == 0xB5 && payload_is_hex(r, "060200000400044c454654"));
  /* long content: TRUNCATED, total_len is the full length */
  uint8_t big[MXP_RESULT_DATA_MAX];
  for (int i = 0; i < MXP_RESULT_DATA_MAX; i++) big[i] = (uint8_t)(255 - i);
  CHECK(mxst_learn_qr(&g_data, big, MXP_RESULT_DATA_MAX, 500) == 3);
  r = get_ldata(&s, 6, 3);
  CHECK(r->cmd == 0xB5 && mxp_unpack_learned_data(r->payload, r->len, &d));
  CHECK(d.flags == MXP_DATA_FLAG_TRUNCATED && d.total_len == 500 && d.len == 128 && d.data[127] == 128);
  /* exactly 128 bytes is not truncated */
  CHECK(mxst_learn_qr(&g_data, big, MXP_RESULT_DATA_MAX, 128) == 4);
  r = get_ldata(&s, 6, 4);
  CHECK(r->cmd == 0xB5 && mxp_unpack_learned_data(r->payload, r->len, &d) && d.flags == 0 && d.len == 128);
  CHECK(g_data.cfg.dirty == 0);
}

static void test_qr_names_and_get_learned(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t six = 6;
  /* SET_NAME 6: only learned ids */
  CHECK(is_err(set_name(&s, 6, 1, "go", 2), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(g_data.n_names == 0 && g_data.cfg.dirty == 0);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"B", 1, 1) == 1);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"A", 1, 1) == 2);
  CHECK(set_name(&s, 6, 2, "\xE5\xB7\xA6", 3)->cmd == 0xB2 && g_data.cfg.dirty == 1); /* 左 */
  CHECK(is_err(set_name(&s, 6, 3, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 6, 0, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  /* GET_LEARNED 6: learned ids in order, rgb 0, names */
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_LEARNED, 2, &six, 1);
  static const uint8_t want[] = { 2, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0xE5, 0xB7, 0xA6 };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want) && memcmp(r->payload, want, sizeof(want)) == 0);
  /* the shared 32-name table applies */
  fresh_m7(&s);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"Q", 1, 1) == 1);
  for (uint16_t i = 0; i < MXST_MAX_NAMES; i++) CHECK(set_name(&s, 2, i, "t", 1)->cmd == 0xB2);
  CHECK(is_err(set_name(&s, 6, 1, "q", 1), MXP_CMD_SET_NAME, MXP_ERR_FULL));
  r = req(&s, MXP_CMD_GET_LEARNED, 3, &six, 1);
  CHECK(r->cmd == 0xB3 && r->len == 7 && r->payload[0] == 1 && r->payload[1] == 1 && r->payload[6] == 0);
}

static void test_forget_qr(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"A", 1, 1) == 1);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"B", 1, 1) == 2);
  CHECK(mxst_set_name(&g_data, 6, 1, "one", 3) && mxst_set_name(&g_data, 6, 2, "two", 3));
  CHECK(mxst_set_name(&g_data, 2, 5, "tag", 3));
  uint32_t gen = s.data_gen;
  forget(&s, 6, 1, 1);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0 && g_data.n_qr == 1 && g_data.qr[0].id == 2);
  CHECK(mxst_get_name(&g_data, 6, 1, NULL) == NULL && mxst_get_name(&g_data, 6, 2, NULL) != NULL);
  CHECK(g_data.cfg.dirty == 1 && s.data_gen == gen + 1);
  /* absent: success, not dirty */
  g_data.cfg.dirty = 0;
  forget(&s, 6, 1, 2);
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0 && s.data_gen == gen + 1);
  forget(&s, 6, 300, 3);
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  /* all: codes and their names, other names stay */
  forget(&s, 6, 0xFFFF, 4);
  CHECK(g_r.cmd == 0xB1 && g_data.n_qr == 0 && g_data.n_names == 1 && mxst_get_name(&g_data, 2, 5, NULL) != NULL);
  CHECK(g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 6, 0xFFFF, 5);
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  /* the learned content is gone too */
  CHECK(is_err(get_ldata(&s, 6, 2), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
}

static void test_shape_names(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t eleven = 11;
  CHECK(mxst_set_name(&g_data, 2, 3, "tag", 3) && mxst_set_name(&g_data, 8, 3, "three", 5));
  CHECK(set_name(&s, 11, 3, "\xE6\x96\xB9", 3)->cmd == 0xB2 && g_data.cfg.dirty == 1); /* 方 */
  CHECK(is_err(set_name(&s, 11, 5, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 11, 0, "x", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(set_name(&s, 11, 4, "rect", 4)->cmd == 0xB2);
  CHECK(set_name(&s, 11, 1, "ball", 4)->cmd == 0xB2);
  /* GET_LEARNED 11: named shapes by id, rgb 0 */
  const mxp_frame_t *r = req(&s, MXP_CMD_GET_LEARNED, 2, &eleven, 1);
  static const uint8_t want[] = { 3, 1, 0, 0, 0, 0, 4, 'b', 'a', 'l', 'l', 3, 0, 0, 0, 0, 3, 0xE6, 0x96, 0xB9,
                                  4, 0, 0, 0, 0, 4, 'r', 'e', 'c', 't' };
  CHECK(r->cmd == 0xB3 && r->len == sizeof(want) && memcmp(r->payload, want, sizeof(want)) == 0);
  /* FORGET 11: one, then all shape names; tag and gesture names stay */
  g_data.cfg.dirty = 0;
  forget(&s, 11, 3, 3);
  CHECK(g_r.cmd == 0xB1 && mxst_get_name(&g_data, 11, 3, NULL) == NULL && g_data.cfg.dirty == 1);
  g_data.cfg.dirty = 0;
  forget(&s, 11, 2, 4); /* absent */
  CHECK(g_r.cmd == 0xB1 && g_data.cfg.dirty == 0);
  forget(&s, 11, 0xFFFF, 5);
  CHECK(g_r.cmd == 0xB1 && g_data.n_names == 2 && mxst_get_name(&g_data, 2, 3, NULL) != NULL &&
        mxst_get_name(&g_data, 8, 3, NULL) != NULL);
  r = req(&s, MXP_CMD_GET_LEARNED, 6, &eleven, 1);
  CHECK(r->cmd == 0xB3 && r->len == 1 && r->payload[0] == 0);
}

static void test_motion_rules(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  uint8_t twelve = 12;
  CHECK(is_err(set_name(&s, 12, 0, "m", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 12, 1, "m", 1), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(is_err(set_name(&s, 12, 1, "", 0), MXP_CMD_SET_NAME, MXP_ERR_BAD_PARAM));
  CHECK(mxst_set_name(&g_data, 2, 1, "tag", 3));
  forget(&s, 12, 0, 1);
  CHECK(g_r.cmd == 0xB1 && g_r.len == 0);
  forget(&s, 12, 0xFFFF, 2);
  CHECK(g_r.cmd == 0xB1 && g_data.n_names == 1 && g_data.cfg.dirty == 0);
  CHECK(is_err(req(&s, MXP_CMD_GET_LEARNED, 3, &twelve, 1), MXP_CMD_GET_LEARNED, MXP_ERR_BAD_PARAM));
}

static void test_factory_reset_m7(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  const uint8_t sp[MXP_FUNC_CONFIG_SIZE] = { 11, 1, 1, 90, 64, 2, 0, 0, 0, 0 };
  const uint8_t mp[MXP_FUNC_CONFIG_SIZE] = { 12, 1, 40, 64, 2, 0, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, sp, sizeof(sp))->cmd == 0xC5 && set_fcfg(&s, mp, sizeof(mp))->cmd == 0xC5);
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(set_name(&s, 6, 1, "l", 1)->cmd == 0xB2 && set_name(&s, 11, 2, "t", 1)->cmd == 0xB2);
  uint8_t cmd = 0, seq = 0, rb[64];
  mxp_frame_t r;
  CHECK(deferred(&s, MXP_CMD_FACTORY_RESET, 30) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, NULL) && mxs_complete_store(&s, 30, true));
  pop_decoded(&s, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xC3);
  CHECK(payload_is_hex(get_fcfg(&s, 11), "0b010000200400000000"));
  CHECK(payload_is_hex(get_fcfg(&s, 12), "0c011918040000000000"));
  CHECK(g_data.n_qr == 0 && g_data.n_names == 0 && g_data.cfg.dirty == 0);
  CHECK(is_err(get_ldata(&s, 6, 1), MXP_CMD_GET_LEARNED_DATA, MXP_ERR_BAD_PARAM));
}

static void test_save_snapshot_includes_qr(void)
{
  mxs_state_t s;
  fresh_m7(&s);
  static mxs_data_t snap;
  CHECK(mxst_learn_qr(&g_data, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(set_name(&s, 6, 1, "left", 4)->cmd == 0xB2);
  const uint8_t sp[MXP_FUNC_CONFIG_SIZE] = { 11, 1, 1, 90, 64, 2, 0, 0, 0, 0 };
  const uint8_t mp[MXP_FUNC_CONFIG_SIZE] = { 12, 1, 40, 64, 2, 0, 0, 0, 0, 0 };
  CHECK(set_fcfg(&s, sp, sizeof(sp))->cmd == 0xC5 && set_fcfg(&s, mp, sizeof(mp))->cmd == 0xC5);
  uint8_t cmd = 0, seq = 0;
  CHECK(deferred(&s, MXP_CMD_SAVE, 31) == MXS_DEFERRED);
  CHECK(mxs_take_pending_store(&s, &cmd, &seq, &snap) && cmd == MXP_CMD_SAVE);
  CHECK(snap.n_qr == 1 && snap.qr[0].id == 1 && snap.qr[0].len == 4 && memcmp(snap.qr[0].bytes, "LEFT", 4) == 0);
  CHECK(snap.shape_cfg.mode == 1 && snap.shape_cfg.threshold == 90 && snap.motion_cfg.threshold == 40);
  /* the snapshot serialises and reads back the same */
  static uint8_t blob[MXST_BLOB_MAX];
  static mxs_data_t back;
  size_t n = mxst_serialize(&snap, blob, sizeof(blob));
  CHECK(n > 0 && mxst_deserialize(blob, n, &back) && back.n_qr == 1 && back.qr[0].total_len == 4);
  CHECK(mxst_get_name(&back, 6, 1, NULL) != NULL && back.motion_cfg.min_size == 64);
  CHECK(mxs_complete_store(&s, 31, true) && g_data.cfg.dirty == 0);
}

int main(void)
{
  test_info();
  test_ping_function();
  test_errors();
  test_stream();
  test_timeout();
  test_learn();
  test_learn_cancel();
  test_results();
  test_forget();
  test_set_name();
  test_get_learned();
  test_config();
  test_save_reset();
  test_finish_learn();
  test_capabilities();
  test_results_on_function_change();
  test_face_function();
  test_dirty_only_on_change();
  test_deferred_order();
  test_get_info_nine_functions();
  test_func_config_gesture_get_default();
  test_func_config_gesture_set_clamps_and_dirty();
  test_func_config_errors();
  test_gesture_names();
  test_learn_gesture_rejected();
  test_factory_reset_gesture_defaults();
  test_save_snapshot_includes_gesture();
  test_get_info_lists_five();
  test_func_config_line_track_default();
  test_func_config_line_track_set();
  test_func_config_line_track_errors();
  test_learn_with_id_rejected();
  test_learn_fn_mismatch();
  test_learn_line_flow();
  test_learn_line_no_color();
  test_learn_object_flow();
  test_forget_line_resets_mode();
  test_forget_object_and_name();
  test_set_name_rules_m6();
  test_get_learned_line_and_object();
  test_factory_reset_m6();
  test_save_snapshot_includes_object();
  test_get_info_lists_m7();
  test_func_config_shape_motion();
  test_learn_qr_flow();
  test_learn_shape_motion_rejected();
  test_learn_11_bytes_still_rejected();
  test_result_data();
  test_learned_data();
  test_qr_names_and_get_learned();
  test_forget_qr();
  test_shape_names();
  test_motion_rules();
  test_factory_reset_m7();
  test_save_snapshot_includes_qr();
  if (g_fail) {
    printf("%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("all %d checks passed\n", g_checks);
  return 0;
}
