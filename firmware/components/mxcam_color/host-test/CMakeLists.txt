cmake_minimum_required(VERSION 3.16)
project(mxcam_color_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(COL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_color test_color.c "${COL_SRC}/mxcam_color.c" "${PROTO_SRC}/mxcam_proto.c")
target_compile_definitions(test_color PRIVATE MXC_TEST_HOOKS)
target_include_directories(test_color PRIVATE "${COL_SRC}" "${PROTO_SRC}")
set_target_properties(test_color PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_color PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_color COMMAND test_color)
