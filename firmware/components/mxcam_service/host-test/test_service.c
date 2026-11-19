#include <stdio.h>
#include <string.h>

#include "mxcam_service.h"

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
  return i;
}

/* Run a request through the service, decode the response with the codec. */
static size_t call(mxs_state_t *s, uint8_t cmd, uint8_t seq, const uint8_t *pl, uint16_t len,
                   uint32_t now, mxp_frame_t *resp, uint8_t *rbuf, size_t rbuf_cap)
{
  uint8_t out[128];
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
  mxs_init(&s, &info);
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
  mxs_init(&s, &info);
  uint8_t rb[64];
  mxp_frame_t r;
  call(&s, MXP_CMD_PING, 42, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x82 && r.len == 0 && r.seq == 42);

  uint8_t zero = 0, one = 1;
  call(&s, MXP_CMD_SET_FUNCTION, 3, &zero, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x90 && r.len == 1 && r.payload[0] == 0 && r.seq == 3);
  call(&s, MXP_CMD_SET_FUNCTION, 4, &one, 1, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.len == 2 && r.payload[0] == MXP_CMD_SET_FUNCTION &&
        r.payload[1] == MXP_ERR_BAD_PARAM && r.seq == 4);
  call(&s, MXP_CMD_SET_FUNCTION, 5, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.payload[1] == MXP_ERR_BAD_LENGTH);
  call(&s, MXP_CMD_GET_FUNCTION, 6, NULL, 0, 0, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0x91 && r.len == 1 && r.payload[0] == 0);
}

static void test_errors(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxs_init(&s, &info);
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
  mxs_init(&s, &info);
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
  mxs_init(&s, &info);
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
  mxs_init(&s, &info);
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
  n = mxs_complete_learn(&s, seq, 4, out, sizeof(out));
  CHECK(n > 0);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xB0 && r.seq == 9 && r.len == 1 && r.payload[0] == 4);
  CHECK(!s.pending_learn.active);

  /* NO_COLOR -> BAD_PARAM, FULL -> FULL */
  CHECK(learn(&s, 1, 20, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  n = mxs_complete_learn(&s, seq, MXS_LEARN_NO_COLOR, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 20 && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM);
  CHECK(learn(&s, 1, 21, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  n = mxs_complete_learn(&s, seq, MXS_LEARN_FULL, out, sizeof(out));
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 21 && r.payload[1] == MXP_ERR_FULL);
  CHECK(!s.pending_learn.active);
  CHECK(mxs_complete_learn(&s, 21, 1, out, sizeof(out)) == 0); /* nothing pending */
}

static void test_learn_cancel(void)
{
  mxp_info_t info = make_info();
  mxs_state_t s;
  mxs_init(&s, &info);
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
  size_t n = mxs_peek_cancelled(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 31);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 31 && r.payload[0] == MXP_CMD_LEARN && r.payload[1] == MXP_ERR_BAD_PARAM);
  /* peek does not consume: a failed send is retried by the next flush */
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) == n && seq == 31);
  mxs_ack_cancelled(&s);
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) == 0);

  /* stream/host timeout cancels it too */
  s.function_id = 1;
  CHECK(learn(&s, 1, 32, 1000, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  mxs_tick(&s, 3999);
  CHECK(s.pending_learn.active);
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) == 0);
  mxs_tick(&s, 4001);
  CHECK(!s.pending_learn.active);
  n = mxs_peek_cancelled(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 32);
  decode(out, n, &r, rb, sizeof(rb));
  CHECK(r.cmd == 0xFF && r.seq == 32 && r.payload[1] == MXP_ERR_BAD_PARAM);
  mxs_ack_cancelled(&s);

  /* a CLAIMED learn is cancelled too (BAD_PARAM queued), so it cannot wedge
   * LEARN as BUSY; a new LEARN is accepted afterwards. */
  s.function_id = 1;
  CHECK(learn(&s, 1, 33, 5000, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  mxp_learn_req_t req;
  CHECK(mxs_take_pending_learn(&s, &seq, &req));
  call(&s, MXP_CMD_SET_FUNCTION, 7, &zero, 1, 5001, &r, rb, sizeof(rb));
  CHECK(!s.pending_learn.active);
  n = mxs_peek_cancelled(&s, &seq, out, sizeof(out));
  CHECK(n > 0 && seq == 33);
  mxs_ack_cancelled(&s);
  CHECK(mxs_complete_learn(&s, 33, 2, out, sizeof(out)) == 0); /* late completion is ignored */
  s.function_id = 1;
  CHECK(learn(&s, 1, 34, 5002, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  CHECK(mxs_complete_learn(&s, 34, 2, out, sizeof(out)) == 0); /* not claimed yet */
  CHECK(s.pending_learn.active);

  /* two cancels before a flush are both delivered, in order */
  mxs_init(&s, &info);
  s.function_id = 1;
  CHECK(learn(&s, 1, 40, 0, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  call(&s, MXP_CMD_SET_FUNCTION, 8, &zero, 1, 1, &r, rb, sizeof(rb));
  s.function_id = 1;
  CHECK(learn(&s, 1, 41, 2, &r, rb, sizeof(rb)) == MXS_DEFERRED);
  call(&s, MXP_CMD_SET_FUNCTION, 9, &zero, 1, 3, &r, rb, sizeof(rb));
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) > 0 && seq == 40);
  mxs_ack_cancelled(&s);
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) > 0 && seq == 41);
  mxs_ack_cancelled(&s);
  CHECK(mxs_peek_cancelled(&s, &seq, out, sizeof(out)) == 0);
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
  if (g_fail) {
    printf("%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("all %d checks passed\n", g_checks);
  return 0;
}
