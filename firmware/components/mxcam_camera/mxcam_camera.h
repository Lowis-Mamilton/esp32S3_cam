#ifndef MXCAM_CAMERA_H
#define MXCAM_CAMERA_H

#include "esp_err.h"
#include "esp_camera.h"

/* OV2640, QVGA 320x240 RGB565, 2 frame buffers in PSRAM, grab-latest mode. */
esp_err_t mxcam_camera_init(void);
/* Latest frame (320x240 RGB565), or NULL on failure. Must be released. */
camera_fb_t *mxcam_camera_grab(void);
void mxcam_camera_release(camera_fb_t *fb);

#endif
