cmake_minimum_required(VERSION 3.16)
project(mxcam_service_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(SVC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_service test_service.c "${SVC_SRC}/mxcam_service.c" "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_service PRIVATE "${SVC_SRC}" "${PROTO_SRC}")
set_target_properties(test_service PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_service PRIVATE -std=c99 -Wall -Wextra -Werror)
add_test(NAME test_service COMMAND test_service)
