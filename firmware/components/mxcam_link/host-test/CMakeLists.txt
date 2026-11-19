cmake_minimum_required(VERSION 3.16)
project(mxcam_pushq_host_test C)
enable_testing()

set(LINK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_pushq test_pushq.c "${LINK_SRC}/pushq.c")
target_include_directories(test_pushq PRIVATE "${LINK_SRC}")
set_target_properties(test_pushq PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_pushq PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_pushq COMMAND test_pushq)
