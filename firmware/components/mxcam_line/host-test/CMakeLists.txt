cmake_minimum_required(VERSION 3.16)
project(mxcam_line_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(LINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GRAY_SRC "${REPO_ROOT}/firmware/components/mxcam_gray")
set(COL_SRC "${REPO_ROOT}/firmware/components/mxcam_color")

# Also covers mxcam_gray (it has no host test of its own).
add_executable(test_line test_line.c "${LINE_SRC}/mxcam_line.c" "${GRAY_SRC}/mxcam_gray.c"
               "${COL_SRC}/mxcam_color.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_line PRIVATE "${LINE_SRC}" "${GRAY_SRC}" "${COL_SRC}" "${PROTO_SRC}")
if(NOT MSVC)
  target_link_libraries(test_line PRIVATE m)
endif()
set_target_properties(test_line PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_line PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_line COMMAND test_line)
