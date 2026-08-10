/*
 * C99 math function declarations missing from the 10.6 SDK's <math.h>.
 * These exist in libSystem (runtime) but the SDK headers don't declare them.
 * libcxx 5.0.1's <cmath> expects them in the global namespace.
 * Force-included via cpp_args only (not c_args — C files don't go through
 * libcxx's wrapper, and -include in c_args breaks assembly files).
 */
#ifndef MATH_COMPAT_H
#define MATH_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* C99 rounding functions */
extern double rint(double x);
extern float rintf(float x);
extern long double rintl(long double x);
extern long long int llrint(double x);
extern long long int llrintf(float x);
extern long long int llrintl(long double x);
extern long long int llround(double x);
extern long long int llroundf(float x);
extern long long int llroundl(long double x);
extern double nearbyint(double x);
extern float nearbyintf(float x);
extern long double nearbyintl(long double x);
extern double trunc(double x);
extern float truncf(float x);
extern long double truncl(long double x);

/* C99 remainder/remquo */
extern double remainder(double x, double y);
extern float remainderf(float x, float y);
extern long double remainderl(long double x, long double y);
extern double remquo(double x, double y, int *quo);
extern float remquof(float x, float y, int *quo);
extern long double remquol(long double x, long double y, int *quo);

/* C99 sign/manipulation */
extern double copysign(double x, double y);
extern float copysignf(float x, float y);
extern long double copysignl(long double x, long double y);

/* C99 positive difference / min / max */
extern double fdim(double x, double y);
extern float fdimf(float x, float y);
extern long double fdiml(long double x, long double y);
extern double fmax(double x, double y);
extern float fmaxf(float x, float y);
extern long double fmaxl(long double x, long double y);
extern double fmin(double x, double y);
extern float fminf(float x, float y);
extern long double fminl(long double x, long double y);

/* C99 fused multiply-add */
extern double fma(double x, double y, double z);
extern float fmaf(float x, float y, float z);
extern long double fmal(long double x, long double y, long double z);

#ifdef __cplusplus
}
#endif

#endif /* MATH_COMPAT_H */
