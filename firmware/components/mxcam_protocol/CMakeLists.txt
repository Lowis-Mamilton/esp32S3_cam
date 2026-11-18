# ADR-0006: reference the shared codec sources directly, no copy.
set(MXP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../arduino-lib/src/protocol")

idf_component_register(SRCS "${MXP_DIR}/mxcam_proto.c"
                       INCLUDE_DIRS "${MXP_DIR}")
