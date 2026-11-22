cmake_minimum_required(VERSION 3.16)
project(mxcam_shape_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(SHAPE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GRAY_SRC "${REPO_ROOT}/firmware/components/mxcam_gray")

add_executable(test_shape test_shape.c "${SHAPE_SRC}/mxcam_shape.c" "${GRAY_SRC}/mxcam_gray.c"
               "${PROTO_SRC}/mxcam_proto.c")
target_compile_definitions(test_shape PRIVATE MXSH_TEST_HOOKS)
target_include_directories(test_shape PRIVATE "${SHAPE_SRC}" "${GRAY_SRC}" "${PROTO_SRC}")
if(NOT MSVC)
  target_link_libraries(test_shape PRIVATE m)
endif()
set_target_properties(test_shape PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_shape PRIVATE -std=c99 -Wall -Wextra -Werror -O2)
add_test(NAME test_shape COMMAND test_shape)
