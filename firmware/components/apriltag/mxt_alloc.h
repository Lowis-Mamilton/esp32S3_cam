/* Not upstream (MXCam): where the vendored apriltag allocates on ESP-IDF.
 * Safe to include anywhere (no macros); mxt_alloc_shim.h does the mapping. */
#ifndef MXT_ALLOC_H
#define MXT_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MXT_ALLOC_PSRAM,   /* default: PSRAM first, internal only as a fallback */
  MXT_ALLOC_DEFAULT, /* plain malloc policy (blocks <= 4 KB internal first) */
} mxt_alloc_mode_t;
void mxt_alloc_set_mode(mxt_alloc_mode_t mode);

void *mxt_alloc_malloc(size_t n);
void *mxt_alloc_calloc(size_t n, size_t size);
void *mxt_alloc_realloc(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
