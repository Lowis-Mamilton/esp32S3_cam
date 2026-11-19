/* MXCam USB link: USB-Serial-JTAG (the "OTG" port) carries the protocol only.
 * Nothing in this component may log to that port (console is UART0). */
#ifndef MXCAM_LINK_H
#define MXCAM_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mxcam_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB-Serial-JTAG driver and starts the RX task on core 0
 * (decode -> mxs_handle -> send response). `state` is owned by the caller and
 * must outlive the link; every access to it, here and elsewhere, must be
 * between mxcam_link_state_lock() and mxcam_link_state_unlock(). */
esp_err_t mxcam_link_start(mxs_state_t *state);

/* The one mutex that guards the mxs_state_t. */
void mxcam_link_state_lock(void);
void mxcam_link_state_unlock(void);

/* Sends one complete packet. Never blocks long: waits at most ~150 ms for the tx
 * mutex (a big push may hold it) and gives up ~50 ms after getting it (e.g.
 * nobody reads the port), returning false; a partial packet may then
 * have been queued, which host decoders resync from via the CRC. */
bool mxcam_link_send(const uint8_t *data, size_t len);

/* Non-blocking push (FRAME 0x60 / RESULTS 0x61): copies the payload into a slot
 * and returns at once (false: bad cmd/len or link not started). FRAME keeps at
 * most 2 pending (the oldest is dropped), RESULTS only the latest. A dedicated
 * TX task (core 0, prio 4) encodes with the link's own push seq (which advances
 * only for pushes actually sent; use frame_id to detect drops) and sends each
 * packet under one tx mutex hold with a length-scaled budget (50 ms + ~1 ms/KB).
 * Hard caps: FRAME payload <= 48 KB (PUSHQ_MAX_FRAME_PAYLOAD; larger is dropped
 * and counted, push returns false), RESULTS <= 512 B. */
bool mxcam_link_push(uint8_t cmd, const uint8_t *payload, uint16_t len);

/* Sends queued deferred responses (mxs_peek_deferred: LEARN results and
 * cancellations, SAVE / FACTORY_RESET replies), acking each after a successful send. The RX task calls it
 * after every request and on idle wakeups (<= 100 ms), the TX task on every
 * wakeup. Blocking (sends); never call it from the pipeline. */
void mxcam_link_flush_deferred(void);
/* Non-blocking: wakes the TX task, which flushes the deferred responses. For
 * the pipeline / store worker after they queued one (mxs_finish_learn,
 * mxs_complete_store). */
void mxcam_link_kick_deferred(void);
/* Drops queued (not yet sending) RESULTS pushes. Call with the state lock held
 * when mxs_take_results_invalidated() reports a function change. */
void mxcam_link_drop_results_locked(void);

/* Since the last call: pushes dropped by the queue policy, dropped for exceeding
 * the size cap, and failed to send. */
void mxcam_link_take_push_stats(uint32_t *dropped, uint32_t *dropped_size, uint32_t *tx_fail);

#ifdef __cplusplus
}
#endif

#endif
