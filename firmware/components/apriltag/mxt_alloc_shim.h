/* Not upstream (MXCam): force-included into every vendored apriltag source on
 * ESP-IDF (see CMakeLists.txt) so its malloc/calloc/realloc go through
 * mxt_alloc_* (mxt_alloc.h) and land in PSRAM instead of following the
 * internal-RAM-first policy plain malloc has for blocks <= 4 KB
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL): upstream makes many small per-frame
 * allocations that otherwise eat ~130 KB of internal RAM at peak. free() needs
 * no mapping: ESP-IDF's free releases blocks from any heap. The vendored files
 * stay byte-identical. */
#ifndef MXT_ALLOC_SHIM_H
#define MXT_ALLOC_SHIM_H

#include "mxt_alloc.h"

/* Function-like on purpose: attribute spellings such as __malloc__ are left
 * alone, and the declarations in <stdlib.h> become matching declarations of
 * the mxt_alloc_* functions. */
#define malloc(n) mxt_alloc_malloc(n)
#define calloc(n, s) mxt_alloc_calloc(n, s)
#define realloc(p, n) mxt_alloc_realloc(p, n)

#endif
