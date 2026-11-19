#ifndef GESTURE_BENCH_H
#define GESTURE_BENCH_H

#ifdef MXCAM_GESTURE_BENCH
#include <stdint.h>

/* On-board gesture recognition benchmark (local builds only:
 * idf.py -DMXCAM_GESTURE_BENCH=ON build, see main/CMakeLists.txt). Call once
 * per pipeline frame with the clean (no overlay) 320x240 RGB565BE frame; once
 * per second it runs mxh_detect(frame, 10, 0, 2, ...) on it and on each
 * embedded test frame and logs one "gesture_bench" line per run. Pipeline
 * task only. */
void gesture_bench_frame(const uint16_t *frame);

#ifdef MXCAM_GESTURE_BENCH_FEED
/* Feed mode (-DMXCAM_GESTURE_BENCH_FEED=ON, needs the test frames): call
 * right after grabbing a camera frame; on a fixed schedule it overwrites the
 * frame with a test frame (logged as "feed: imgN" / "feed: live"), so the
 * normal pipeline (TFT, RESULTS, FRAME) runs on known hands. Replaces
 * gesture_bench_frame (no stalling benchmark runs). */
void gesture_bench_feed(uint16_t *frame);
#endif
#endif

#endif
