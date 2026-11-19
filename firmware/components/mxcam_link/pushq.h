/* Slot policy for the non-blocking push queue (pure C, no RTOS).
 *
 * A queue has n slots. A slot is FREE, FILLING (producer is copying into it),
 * READY (queued) or SENDING (consumer owns it). At most `max_pending` slots are
 * READY: when a new entry arrives and that many are already queued, the oldest
 * READY slot is reused ("drop oldest"). With max_pending == 1 this is
 * "latest wins". A SENDING slot is never touched. Use n = max_pending + 1 so a
 * slot is always available to a single producer.
 *
 * NOT thread safe: callers wrap each call in one critical section (the calls
 * are a few instructions each); the payload copy happens outside the lock. */
#ifndef MXCAM_PUSHQ_H
#define MXCAM_PUSHQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUSHQ_MAX_SLOTS 4

/* Hard cap for a pushed FRAME payload (header + JPEG). Larger frames are
 * dropped (counted) by the link; the encoder lowers quality instead of exceeding it. */
#define PUSHQ_MAX_FRAME_PAYLOAD (48u * 1024u)

/* Time budget for sending one whole push packet of len bytes: 50 ms base plus
 * about 1 ms per KB (USB full speed moves ~1 KB/ms at best). */
uint32_t pushq_send_budget_us(size_t len);

typedef struct {
  uint8_t n, max_pending;
  uint8_t state[PUSHQ_MAX_SLOTS];
  uint32_t order[PUSHQ_MAX_SLOTS]; /* commit order, valid while READY */
  uint32_t counter;
} pushq_t;

void pushq_init(pushq_t *q, uint8_t n, uint8_t max_pending);
/* Reserve a slot for filling; *dropped is set when a queued entry was discarded
 * to make room. Returns -1 if no slot is available (only with >1 producer). */
int pushq_acquire(pushq_t *q, bool *dropped);
void pushq_commit(pushq_t *q, int idx); /* FILLING -> READY */
void pushq_abort(pushq_t *q, int idx);  /* FILLING -> FREE */
int pushq_take(pushq_t *q);             /* oldest READY -> SENDING, or -1 */
void pushq_done(pushq_t *q, int idx);   /* SENDING -> FREE */
uint8_t pushq_pending(const pushq_t *q); /* READY count */
/* Discards every READY entry (e.g. stale RESULTS after a function change);
 * SENDING and FILLING slots are untouched. Returns how many were dropped. */
uint8_t pushq_drop_pending(pushq_t *q);

#ifdef __cplusplus
}
#endif

#endif
