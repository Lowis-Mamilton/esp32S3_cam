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
  /* face (3): nothing is learned or named, so FORGET succeeds (see test_face_function) */
  forget(&s, 4, 1, 9);
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
  if (g_fail) {
    printf("%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("all %d checks passed\n", g_checks);
  return 0;
}
