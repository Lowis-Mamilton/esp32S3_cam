# Vendored AprilRobotics apriltag (see README.md): tag36h11 only, upstream files
# unmodified. Source list shared with the mxcam_tag host test.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

idf_component_register(SRCS ${APRILTAG_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES pthread)

# Upstream builds with -Wno-shift-negative-value. -Wno-format: its debug/PNM
# writers print int32_t with %d, which is long on Xtensa (-Werror=format under
# IDF). Silenced here instead of editing upstream code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-shift-negative-value)
# IDF already passes -D_GNU_SOURCE; workerpool.c defines it again (harmless
# "redefined" warning). Undefine on the command line so the build stays quiet.
set_source_files_properties(common/workerpool.c PROPERTIES COMPILE_OPTIONS "-U_GNU_SOURCE")
