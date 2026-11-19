cmake_minimum_required(VERSION 3.16)
project(mxcam_tag_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(TAG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(AT_DIR "${REPO_ROOT}/firmware/components/apriltag")

# Vendored upstream: same source list as the ESP-IDF component, built with the
# compiler defaults (no -std=c99 / -Werror; it is not our code). Its headers
# are SYSTEM includes so they do not trip our warning flags.
include("${AT_DIR}/sources.cmake")
list(TRANSFORM APRILTAG_SRCS PREPEND "${AT_DIR}/")
add_library(apriltag_vendor STATIC ${APRILTAG_SRCS})
# Same single-precision build as on the target (double -> float shim); the
# test and the wrapper see the same struct layout.
set(MXT_FLOAT_SHIM "${AT_DIR}/mxt_float_shim.h")
target_compile_options(apriltag_vendor PRIVATE -include "${MXT_FLOAT_SHIM}" -fsingle-precision-constant)
target_include_directories(apriltag_vendor SYSTEM PUBLIC "${AT_DIR}")
if(NOT WIN32)
  # On Windows the vendored pthreads_cross.c provides the pthread API itself.
  find_package(Threads REQUIRED)
  target_link_libraries(apriltag_vendor PUBLIC Threads::Threads m)
endif()

add_executable(test_tag test_tag.c "${TAG_SRC}/mxcam_tag.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_tag PRIVATE "${TAG_SRC}" "${PROTO_SRC}")
target_link_libraries(test_tag PRIVATE apriltag_vendor)
set_target_properties(test_tag PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_tag PRIVATE -std=c99 -Wall -Wextra -Werror -include "${MXT_FLOAT_SHIM}")
if(NOT WIN32)
  # Strict -std=c99 hides POSIX declarations that the vendored headers
  # (pthread / time) rely on under glibc.
  target_compile_definitions(test_tag PRIVATE _POSIX_C_SOURCE=200809L)
endif()
add_test(NAME test_tag COMMAND test_tag)
