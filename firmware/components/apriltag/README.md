# apriltag (vendored)

Minimal subset of [AprilRobotics/apriltag](https://github.com/AprilRobotics/apriltag),
BSD 2-Clause (see `LICENSE.md`), used by `mxcam_tag` (ADR-0008).

- Upstream tag: **v3.4.5**
- Upstream commit: **94be783968e5091bcc9972c72c84fd63efce2935**
- Upstream files are copied **unmodified**.

## What is included

Only what `apriltag_detector_detect()` with the tag36h11 family needs to compile
and link:

- `apriltag.c/.h`, `apriltag_math.h`, `apriltag_quad_thresh.c`
- `tag36h11.c/.h` (no other families)
- `common/`: `debug_print.h`, `g2d`, `homography`, `image_types.h`, `image_u8`,
  `image_u8_parallel`, `image_u8x3`, `matd`, `math_util.h`, `pnm`,
  `postscript_utils.h`, `pthreads_cross`, `svd22`, `time_util`, `timeprofile.h`,
  `unionfind`, `workerpool`, `zarray`, `zmaxheap`

`image_u8x3`, `pnm` and `postscript_utils.h` are only reached by upstream's
debug/image-file code paths, but `apriltag.c` / `image_u8.c` reference them, so
they are kept to link. Excluded: pose estimation, other tag families, Python
wrapper, examples, `getopt`, `pjpeg`, `pam`, `zhash`, `string_util`,
`image_u8x4`, `doubles`/`floats`.

## Local (non-upstream) files

- `sources.cmake`: the source list, shared by `CMakeLists.txt` and the
  `mxcam_tag` host test.
- `CMakeLists.txt`: ESP-IDF component. Compiler flags instead of code edits:
  `-Wno-format` (upstream prints `int32_t` with `%d`; it is `long` on Xtensa),
  `-Wno-shift-negative-value` (upstream sets this too), and `-U_GNU_SOURCE` for
  `workerpool.c` (it redefines the macro IDF already passes).

No shims: ESP-IDF's `pthread` component and newlib provide everything
`pthreads_cross` / `workerpool` / `time_util` use. `mxcam_tag` runs with
`nthreads = 1`, so `workerpool` never creates a thread.

## Memory

Upstream uses plain `malloc`. `sdkconfig.defaults` sets
`CONFIG_SPIRAM_USE_MALLOC=y` and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, so
anything above 4 KB comes from PSRAM. `mxcam_tag` builds the hamming<=1 decode
table (~1 MB) instead of upstream's default hamming<=2 table (~36 MB for
tag36h11).

## Updating

Clone the new tag outside the repo, copy the same file list over, update the tag
and commit above, rebuild (`idf.py build`) and rerun the `mxcam_tag` host test.
