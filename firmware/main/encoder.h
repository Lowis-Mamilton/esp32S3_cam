/* JPEG encoder task for the FRAME push stream (espressif/esp_new_jpeg). Runs on
 * core 0 at a lower priority than the link so encoding never delays responses;
 * the pipeline (core 1) only copies a frame in. */
#ifndef MXCAM_ENCODER_H
#define MXCAM_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    unsigned frames;    /* FRAME pushes queued since the last take */
    unsigned send_fail; /* pushes the link refused */
    unsigned enc_fail;  /* encoder errors */
    unsigned reenc;     /* JPEGs over the 48 KB push cap re-encoded at lower quality */
    unsigned too_big;   /* still too big after the re-encode: dropped */
    uint32_t enc_avg_ms, enc_max_ms; /* per pushed frame, including a re-encode */
    uint32_t jpeg_avg;  /* bytes */
    uint32_t stack_hw;  /* encoder task stack high-water mark (bytes left) */
} encoder_stats_t;

esp_err_t encoder_start(void);

/* Non-blocking. If the encoder is idle, copies the 320x240 RGB565 (big-endian
 * bytes, as the camera delivers) frame into the staging buffer and wakes the
 * encoder; returns false (and copies nothing) when it is still busy with the
 * previous frame or was never started. quality is 1-100. */
bool encoder_try_submit(const uint16_t *rgb565, uint32_t frame_id, uint32_t timestamp_ms, uint8_t quality);

/* Returns the statistics since the previous call and resets them. */
void encoder_take_stats(encoder_stats_t *out);

#endif
