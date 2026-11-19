/* Not upstream (MXCam): force-included (after mxt_alloc_shim.h) into every
 * vendored apriltag source AND into mxcam_tag.c / its host test, so the
 * detector computes in single precision. The ESP32-S3 FPU only does float;
 * double is emulated in software and dominated quad fitting, edge refinement
 * and decoding (measured on the board, see the Task 6 report).
 * The system headers are included first so their prototypes keep the real
 * double types; afterwards `double` means float and the libm calls map to
 * their float versions. Everything that sees apriltag's structs must be
 * compiled with this header (same struct layout). Vendored files stay
 * byte-identical. */
#ifndef MXT_FLOAT_SHIM_H
#define MXT_FLOAT_SHIM_H

/* Every system header the vendored sources include, so none is parsed after
 * the remapping below. */
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* Some libcs (MinGW) spell `long double` inside these classification macros. */
#undef isfinite
#undef isnan
#undef isinf
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)

#define double float
#define sqrt(x) sqrtf(x)
#define fabs(x) fabsf(x)
#define atan2(y, x) atan2f(y, x)
#define cos(x) cosf(x)
#define sin(x) sinf(x)
#define tan(x) tanf(x)
#define floor(x) floorf(x)
#define ceil(x) ceilf(x)
#define exp(x) expf(x)
#define log(x) logf(x)
#define fmin(a, b) fminf(a, b)
#define fmax(a, b) fmaxf(a, b)
#define fmod(a, b) fmodf(a, b)
#define modf(x, ip) modff(x, ip)

#endif
