#ifndef MXCAM_PIPELINE_H
#define MXCAM_PIPELINE_H

/* Starts the camera -> overlay -> display task on core 1. Camera and display must be initialised. */
void pipeline_start(void);

#endif
