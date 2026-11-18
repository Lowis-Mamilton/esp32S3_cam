/* JPEG encoder task for the FRAME push stream. Runs on core 0 at a lower
 * priority than the pipeline so encoding never slows the TFT updates. */
#ifndef MXCAM_ENCODER_H
#define MXCAM_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    unsigned frames;      /* frames sent successfully since the last take */
    unsigned send_fail;   /* packets mxcam_link_send gave up on */
    unsigned enc_fail;    /* fmt2jpg failures or oversized JPEGs */
    uint32_t enc_avg_ms, enc_max_ms;
    uint32_t jpeg_avg;    /* bytes */
} encoder_stats_t;

esp_err_t encoder_start(void);

/* Non-blocking. If the encoder is idle, copies the 320x240 RGB565 frame into
 * the staging buffer and wakes the encoder; returns false (and copies nothing)
 * when it is still busy with the previous frame. */
bool encoder_try_submit(const uint16_t *rgb565, uint32_t frame_id, uint32_t timestamp_ms, uint8_t quality);

/* Returns the statistics since the previous call and resets them. */
void encoder_take_stats(encoder_stats_t *out);

#endif
