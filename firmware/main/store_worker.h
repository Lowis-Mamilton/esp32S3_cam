/* Save worker: performs deferred SAVE / FACTORY_RESET (NVS) outside the state
 * lock and sends their responses. Core 0, low priority. */
#ifndef MXCAM_STORE_WORKER_H
#define MXCAM_STORE_WORKER_H

#include <stdbool.h>

#include "esp_err.h"
#include "mxcam_service.h"

/* `state` is guarded by mxcam_link_state_lock(); the link must be started.
 * With `tick` the worker also enforces the stream/host timeout (mxs_tick) —
 * used when the pipeline (which normally does it) is not running. */
esp_err_t store_worker_start(mxs_state_t *state, bool tick);

#endif
