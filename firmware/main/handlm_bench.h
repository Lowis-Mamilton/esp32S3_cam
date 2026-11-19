#ifndef HANDLM_BENCH_H
#define HANDLM_BENCH_H

#ifdef MXCAM_HANDLM_BENCH
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hand landmark prototype, phase B (docs/research/hand-landmarks-proto-b.md).
 * Local builds only: idf.py -DMXCAM_HANDLM_BENCH=ON build (main/CMakeLists.txt).
 * The models and test data are NOT in the app image: tools/handlm/pack_storage.py
 * packs them into the `storage` partition (written with esptool).
 *
 * handlm_bench_boot: in app_main right after mxh_init, before any task is
 * created; loads the first model of `storage` with esp-dl's flash-partition
 * location inside the ADR-0011 malloc->PSRAM window and logs time / memory.
 * handlm_bench_frame: once per pipeline frame with the clean 320x240 RGB565BE
 * frame; does one step of the benchmark (one model run, load, test, ...), so
 * the pipeline (and the JPEG encoder on core 0) keep running between steps. */
void handlm_bench_boot(void);
void handlm_bench_frame(const uint16_t *frame);

#ifdef __cplusplus
}
#endif
#endif

#endif
