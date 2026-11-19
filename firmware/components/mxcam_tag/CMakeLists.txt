# Thin wrapper over the vendored apriltag component (ADR-0008); pure C apart
# from esp_timer / heap caps, so it also builds in the host test.
idf_component_register(SRCS "mxcam_tag.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol apriltag
                       PRIV_REQUIRES esp_timer heap)
# Must match the vendored apriltag build: apriltag's structs (detections) are
# compiled with double -> float (apriltag/mxt_float_shim.h).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/../apriltag/mxt_float_shim.h")
