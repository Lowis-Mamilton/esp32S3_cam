cmake_minimum_required(VERSION 3.16)
project(mxcam_host_test C)
enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(VECTORS_JSON "${REPO_ROOT}/docs/protocol-vectors.json")
set(VECTORS_H "${CMAKE_CURRENT_BINARY_DIR}/vectors.h")

add_custom_command(
  OUTPUT "${VECTORS_H}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_vectors.py" "${VECTORS_JSON}" "${VECTORS_H}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_vectors.py" "${VECTORS_JSON}"
  VERBATIM)

add_executable(test_proto test_proto.c "${PROTO_SRC}/mxcam_proto.c" "${VECTORS_H}")
target_include_directories(test_proto PRIVATE "${PROTO_SRC}" "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(test_proto PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_proto PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_proto COMMAND test_proto)
