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

/* Sends one complete packet. Never blocks long: gives up after ~50 ms in total
 * (e.g. nobody reads the port) and returns false; a partial packet may then
 * have been queued, which host decoders resync from via the CRC. */
bool mxcam_link_send(const uint8_t *data, size_t len);

/* Non-blocking push (FRAME 0x60 / RESULTS 0x61): copies the payload into a slot
 * and returns at once (false: bad cmd/len or link not started). FRAME keeps at
 * most 2 pending (the oldest is dropped), RESULTS only the latest. A dedicated
 * TX task (core 0, prio 4) encodes with the link's own push seq and sends. */
bool mxcam_link_push(uint8_t cmd, const uint8_t *payload, uint16_t len);

/* Sends a response packet (cmd is the response byte, e.g. 0xB0) through the
 * budgeted path; for deferred responses, callable from any task. Payload <= 100 B. */
void mxcam_link_respond(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len);

/* Sends the BAD_PARAM for a cancelled deferred LEARN, if any. The RX task calls
 * this itself; call it too after anything that may cancel (e.g. mxs_tick). */
void mxcam_link_flush_cancelled(void);

/* Pushes discarded by the queue policy / failed to send since the last call. */
void mxcam_link_take_push_stats(uint32_t *dropped, uint32_t *tx_fail);

#ifdef __cplusplus
}
#endif

#endif
