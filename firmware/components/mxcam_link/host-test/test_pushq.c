#include <stdio.h>

#include "pushq.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                          \
  do {                                                    \
    g_checks++;                                           \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      g_fail++;                                           \
    }                                                     \
  } while (0)

/* Acquire + commit one entry, returns the slot index. */
static int put(pushq_t *q, bool *dropped)
{
  int i = pushq_acquire(q, dropped);
  CHECK(i >= 0);
  if (i >= 0) pushq_commit(q, i);
  return i;
}

static void test_frame_policy(void)
{
  pushq_t q;
  pushq_init(&q, 3, 2); /* FRAME: sending + 2 pending */
  bool d;
  CHECK(pushq_take(&q) < 0);
  int a = put(&q, &d);
  CHECK(!d);
  int b = put(&q, &d);
  CHECK(!d);
  CHECK(pushq_pending(&q) == 2);
  int c = put(&q, &d);
  CHECK(d); /* third drops the oldest (a) */
  CHECK(c == a && pushq_pending(&q) == 2);
  CHECK(pushq_take(&q) == b); /* b is now the oldest */
  CHECK(pushq_take(&q) == c);
  CHECK(pushq_take(&q) < 0);
  pushq_done(&q, b);
  pushq_done(&q, c);
  CHECK(pushq_pending(&q) == 0);
}

static void test_never_overwrites_sending(void)
{
  pushq_t q;
  pushq_init(&q, 3, 2);
  bool d;
  put(&q, &d);
  int s = pushq_take(&q); /* sending */
  int b = put(&q, &d);
  int c = put(&q, &d);
  CHECK(!d && b != s && c != s);
  for (int k = 0; k < 10; k++) {
    int x = put(&q, &d);
    CHECK(x != s);
  }
  pushq_done(&q, s);
  CHECK(pushq_pending(&q) == 2);
}

static void test_latest_wins(void)
{
  pushq_t q;
  pushq_init(&q, 2, 1); /* RESULTS: only the latest */
  bool d;
  int a = put(&q, &d);
  CHECK(!d);
  int b = put(&q, &d);
  CHECK(d && b == a); /* replaced */
  CHECK(pushq_pending(&q) == 1);
  int s = pushq_take(&q);
  CHECK(s == a);
  int c = put(&q, &d); /* while sending: goes to the other slot */
  CHECK(!d && c != s);
  int e = put(&q, &d); /* replaces c, never s */
  CHECK(d && e == c);
  pushq_done(&q, s);
  CHECK(pushq_take(&q) == c);
}

static void test_abort_and_order(void)
{
  pushq_t q;
  pushq_init(&q, 3, 2);
  bool d;
  int a = pushq_acquire(&q, &d);
  CHECK(a >= 0);
  CHECK(pushq_pending(&q) == 0); /* filling is invisible to the consumer */
  CHECK(pushq_take(&q) < 0);
  pushq_abort(&q, a);
  int x = put(&q, &d), y = put(&q, &d);
  CHECK(pushq_take(&q) == x && pushq_take(&q) == y);
  /* order counter wraps safely */
  q.counter = 0xFFFFFFFEu;
  pushq_done(&q, x);
  pushq_done(&q, y);
  int p = put(&q, &d), r = put(&q, &d);
  CHECK(pushq_take(&q) == p && pushq_take(&q) == r);
}

static void test_budget(void)
{
  CHECK(pushq_send_budget_us(0) == 50000u);
  CHECK(pushq_send_budget_us(1024) == 51000u);
  CHECK(pushq_send_budget_us(PUSHQ_MAX_FRAME_PAYLOAD + 9) == 50000u + (PUSHQ_MAX_FRAME_PAYLOAD + 9) * 1000u / 1024u);
  CHECK(pushq_send_budget_us(2048) > pushq_send_budget_us(1024));
  CHECK(PUSHQ_MAX_FRAME_PAYLOAD == 48 * 1024);
}

int main(void)
{
  test_frame_policy();
  test_never_overwrites_sending();
  test_latest_wins();
  test_abort_and_order();
  test_budget();
  if (g_fail) {
    printf("%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("all %d checks passed\n", g_checks);
  return 0;
}
