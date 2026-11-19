# Not upstream: the subset of AprilRobotics apriltag compiled for MXCam
# (tag36h11 only, no pose, no image I/O tools). Shared by the ESP-IDF component
# (CMakeLists.txt) and the mxcam_tag host test. Paths are relative to this dir.
set(APRILTAG_SRCS
    apriltag.c
    apriltag_quad_thresh.c
    tag36h11.c
    common/g2d.c
    common/homography.c
    common/image_u8.c
    common/image_u8_parallel.c
    common/image_u8x3.c
    common/matd.c
    common/pnm.c
    common/pthreads_cross.c
    common/svd22.c
    common/time_util.c
    common/unionfind.c
    common/workerpool.c
    common/zarray.c
    common/zmaxheap.c)
