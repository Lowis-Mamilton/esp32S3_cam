#include <stddef.h>
#include "pushq.h"

enum { S_FREE = 0, S_FILLING, S_READY, S_SENDING };

void pushq_init(pushq_t *q, uint8_t n, uint8_t max_pending)
{
  if (n > PUSHQ_MAX_SLOTS) n = PUSHQ_MAX_SLOTS;
  q->n = n;
  q->max_pending = max_pending;
  q->counter = 0;
  for (int i = 0; i < PUSHQ_MAX_SLOTS; i++) {
    q->state[i] = S_FREE;
    q->order[i] = 0;
  }
}

uint8_t pushq_pending(const pushq_t *q)
{
  uint8_t c = 0;
  for (int i = 0; i < q->n; i++)
    if (q->state[i] == S_READY) c++;
  return c;
}

uint8_t pushq_drop_pending(pushq_t *q)
{
  uint8_t c = 0;
  for (int i = 0; i < q->n; i++)
    if (q->state[i] == S_READY) {
      q->state[i] = S_FREE;
      c++;
    }
  return c;
}

/* Oldest READY slot (wrap-safe), or -1. */
static int oldest_ready(const pushq_t *q)
{
  int best = -1;
  for (int i = 0; i < q->n; i++) {
    if (q->state[i] != S_READY) continue;
    if (best < 0 || (int32_t)(q->order[i] - q->order[best]) < 0) best = i;
  }
  return best;
}

int pushq_acquire(pushq_t *q, bool *dropped)
{
  *dropped = false;
  if (pushq_pending(q) >= q->max_pending) {
    int o = oldest_ready(q);
    if (o >= 0) {
      q->state[o] = S_FILLING;
      *dropped = true;
      return o;
    }
  }
  for (int i = 0; i < q->n; i++) {
    if (q->state[i] == S_FREE) {
      q->state[i] = S_FILLING;
      return i;
    }
  }
  return -1;
}

void pushq_commit(pushq_t *q, int idx)
{
  if (idx < 0 || idx >= q->n || q->state[idx] != S_FILLING) return;
  q->order[idx] = q->counter++;
  q->state[idx] = S_READY;
}

void pushq_abort(pushq_t *q, int idx)
{
  if (idx < 0 || idx >= q->n || q->state[idx] != S_FILLING) return;
  q->state[idx] = S_FREE;
}

int pushq_take(pushq_t *q)
{
  int o = oldest_ready(q);
  if (o >= 0) q->state[o] = S_SENDING;
  return o;
}

void pushq_done(pushq_t *q, int idx)
{
  if (idx < 0 || idx >= q->n || q->state[idx] != S_SENDING) return;
  q->state[idx] = S_FREE;
}

uint32_t pushq_send_budget_us(size_t len)
{
  return 50000u + (uint32_t)(len * 1000u / 1024u);
}
