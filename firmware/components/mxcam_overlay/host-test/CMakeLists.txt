cmake_minimum_required(VERSION 3.16)
project(mxcam_overlay_host_test C)
enable_testing()

set(OVL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_overlay test_overlay.c "${OVL_SRC}/mxcam_overlay.c")
target_include_directories(test_overlay PRIVATE "${OVL_SRC}")
set_target_properties(test_overlay PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_overlay PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_overlay COMMAND test_overlay)
