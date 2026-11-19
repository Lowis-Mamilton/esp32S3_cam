cmake_minimum_required(VERSION 3.16)
project(mxcam_gesture_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(GESTURE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Only the pure C post-processing is tested here; the esp-dl wrapper
# (mxcam_gesture.cpp) needs the target and is measured on the board.
add_executable(test_gesture_post test_gesture_post.c "${GESTURE_SRC}/mxcam_gesture_post.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_gesture_post PRIVATE "${GESTURE_SRC}" "${PROTO_SRC}")
set_target_properties(test_gesture_post PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_gesture_post PRIVATE -std=c99 -Wall -Wextra -Werror)
if(NOT WIN32)
  target_link_libraries(test_gesture_post PRIVATE m)
endif()
add_test(NAME test_gesture_post COMMAND test_gesture_post)
