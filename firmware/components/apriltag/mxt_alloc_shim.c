/* Not upstream (MXCam): see mxt_alloc_shim.h. This file is force-included with
 * the shim like every apriltag source, so <stdlib.h> declared the mxt_alloc_*
 * names: undo the mapping and declare the real allocator functions here. */
#include "mxt_alloc.h"

#include <stddef.h>

#include "esp_heap_caps.h"

#undef malloc
#undef calloc
#undef realloc
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define ANY_CAPS (MALLOC_CAP_DEFAULT)

static volatile mxt_alloc_mode_t s_mode = MXT_ALLOC_PSRAM;

void mxt_alloc_set_mode(mxt_alloc_mode_t mode) { s_mode = mode; }

void *mxt_alloc_malloc(size_t n)
{
    if (s_mode == MXT_ALLOC_DEFAULT) return malloc(n);
    return heap_caps_malloc_prefer(n, 2, PSRAM_CAPS, ANY_CAPS);
}

void *mxt_alloc_calloc(size_t n, size_t size)
{
    if (s_mode == MXT_ALLOC_DEFAULT) return calloc(n, size);
    return heap_caps_calloc_prefer(n, size, 2, PSRAM_CAPS, ANY_CAPS);
}

void *mxt_alloc_realloc(void *p, size_t n)
{
    if (s_mode == MXT_ALLOC_DEFAULT) return realloc(p, n);
    return heap_caps_realloc_prefer(p, n, 2, PSRAM_CAPS, ANY_CAPS);
}
