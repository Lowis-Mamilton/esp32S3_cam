/* BLE host port for MXCam Studio (ADR-0022). Only compiled into builds with
 * NimBLE enabled (sdkconfig.ble); callers guard with CONFIG_BT_NIMBLE_ENABLED.
 * Logs go to the console (UART0) only. */
#ifndef MXCAM_BLE_H
#define MXCAM_BLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the BLE controller and host, registers the GATT service, adds the
 * port to the link (mxcam_link_add_port) and starts advertising as
 * "MXCam-XXXX" (last two bytes of the Bluetooth address). Call once, after
 * mxcam_link_start and after mxf_init / mxh_init (while those load models,
 * plain malloc goes to PSRAM, and the controller needs internal RAM).
 * Costs about 75 KB of internal RAM. */
esp_err_t mxcam_ble_start(void);

/* The advertised name, e.g. "MXCam-7122" ("" before mxcam_ble_start). */
const char *mxcam_ble_name(void);

#ifdef __cplusplus
}
#endif

#endif
