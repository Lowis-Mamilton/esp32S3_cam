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

int main(void)
{
  test_info();
  test_ping_function();
  test_errors();
  test_stream();
  test_timeout();
  if (g_fail) {
    printf("%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("all %d checks passed\n", g_checks);
  return 0;
}
