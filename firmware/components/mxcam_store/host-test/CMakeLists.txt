cmake_minimum_required(VERSION 3.16)
project(mxcam_store_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(COL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../mxcam_color")
set(ST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# store_nvs.c is ESP-only and intentionally not built here.
add_executable(test_store test_store.c "${ST_SRC}/mxcam_store.c" "${COL_SRC}/mxcam_color.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_store PRIVATE "${ST_SRC}" "${COL_SRC}" "${PROTO_SRC}")
set_target_properties(test_store PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_store PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_store COMMAND test_store)
