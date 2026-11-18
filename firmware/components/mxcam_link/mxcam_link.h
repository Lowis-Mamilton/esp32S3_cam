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

#ifdef __cplusplus
}
#endif

#endif
