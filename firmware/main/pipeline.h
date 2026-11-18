#ifndef MXCAM_PIPELINE_H
#define MXCAM_PIPELINE_H

#include "mxcam_service.h"

/* Starts the camera -> overlay -> display task on core 1. Camera, display, link and encoder must be initialised.
 * `state` is guarded by mxcam_link_state_lock(). */
void pipeline_start(mxs_state_t *state);

#endif
