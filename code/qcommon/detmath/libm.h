/*
 * libm.h: the parts of musl's internal libm.h that the files in this
 * directory use, plus the renames that make them the engine's own
 * deterministic math (Q_det*), separate from the host libm.
 *
 * The .c files here are unmodified copies of musl's src/math (MIT, see
 * COPYRIGHT.musl). A wasmcart's libc IS musl, so with these the native build
 * computes exactly what the cart does.
 */
#ifndef DETMATH_LIBM_H
#define DETMATH_LIBM_H

#include <float.h>
#include <math.h>
#include <stdint.h>

#define sin                Q_detSin
#define cos                Q_detCos
#define atan               Q_detAtan
#define atan2              Q_detAtan2
#define acos               Q_detAcos
#define __sin              Q_det__sin
#define __cos              Q_det__cos
#define __rem_pio2         Q_det__rem_pio2
#define __rem_pio2_large   Q_det__rem_pio2_large

double Q_detSin( double x );
double Q_detCos( double x );
double Q_detAtan( double x );
double Q_detAtan2( double y, double x );
double Q_detAcos( double x );
double Q_det__sin( double x, double y, int iy );
double Q_det__cos( double x, double y );
int Q_det__rem_pio2( double x, double *y );
int Q_det__rem_pio2_large( double *x, double *y, int e0, int nx, int prec );

#define predict_true(x)  __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		volatile float __x;               \
		__x = (x);                        \
		(void)__x;                        \
	} else {                                  \
		volatile double __x;              \
		__x = (x);                        \
		(void)__x;                        \
	}                                         \
} while(0)

#define asuint64(f) ((union{double _f; uint64_t _i;}){f})._i
#define asdouble(i) ((union{uint64_t _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
	uint64_t __u = asuint64(d);               \
	(hi) = __u >> 32;                         \
	(lo) = (uint32_t)__u;                     \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
	(hi) = asuint64(d) >> 32;                 \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
	(lo) = (uint32_t)asuint64(d);             \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
	(d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
	INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
	INSERT_WORDS(d, asuint64(d)>>32, lo)

#endif
