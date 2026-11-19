# Vendored AprilRobotics apriltag (see README.md): tag36h11 only, upstream files
# unmodified. Source list shared with the mxcam_tag host test.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

# mxt_alloc_shim.c (not upstream) routes upstream's allocations (ESP-IDF only).
idf_component_register(SRCS ${APRILTAG_SRCS} "mxt_alloc_shim.c"
                       INCLUDE_DIRS "."
                       REQUIRES pthread heap)

# Upstream builds with -Wno-shift-negative-value. -Wno-format: its debug/PNM
# writers print int32_t with %d, which is long on Xtensa (-Werror=format under
# IDF). Silenced here instead of editing upstream code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-shift-negative-value)
# Every source sees mxt_alloc_shim.h first (malloc/calloc/realloc ->
# mxt_alloc_*, PSRAM), then mxt_float_shim.h (double -> float; the S3 has a
# single-precision FPU only). mxcam_tag.c and the host test use the float shim
# too, so all users agree on the struct layout.
# (SHELL: keeps CMake from de-duplicating the repeated -include.)
target_compile_options(${COMPONENT_LIB} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/mxt_alloc_shim.h"
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/mxt_float_shim.h"
    -fsingle-precision-constant)
# IDF already passes -D_GNU_SOURCE; workerpool.c defines it again (harmless
# "redefined" warning). Undefine on the command line so the build stays quiet.
set_source_files_properties(common/workerpool.c PROPERTIES COMPILE_OPTIONS "-U_GNU_SOURCE")
