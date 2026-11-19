#ifndef MXCAM_PIPELINE_H
#define MXCAM_PIPELINE_H

#include "esp_err.h"
#include "mxcam_service.h"

/* Starts the camera -> recognition -> overlay -> display task on core 1.
 * Camera, display and link must be initialised (encoder and mxt_init are
 * optional: without them FRAME pushes / AprilTag results are simply absent).
 * `state` is guarded by mxcam_link_state_lock(). boot_error (optional, ASCII,
 * static) is shown on the TFT on every frame. */
esp_err_t pipeline_start(mxs_state_t *state, const char *boot_error);

#endif
