cmake_minimum_required(VERSION 3.16)
project(mxcam_qr_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(QR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Only the pure C post-processing is tested here; the quirc wrapper
# (mxcam_qr.c) needs the target and is measured on the board.
add_executable(test_qr_post test_qr_post.c "${QR_SRC}/mxcam_qr_post.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_qr_post PRIVATE "${QR_SRC}" "${PROTO_SRC}")
set_target_properties(test_qr_post PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_qr_post PRIVATE -std=c99 -Wall -Wextra -Werror)
if(NOT WIN32)
  target_link_libraries(test_qr_post PRIVATE m)
endif()
add_test(NAME test_qr_post COMMAND test_qr_post)
