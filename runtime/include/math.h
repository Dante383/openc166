/* SPDX-License-Identifier: MIT */
/* <math.h> of openc166's C library. The functions are LLVM libc's (see
   runtime/README.md), double and float versions; with -mdouble=32
   (_SINGLE_FP), where double is float, each double name is the float
   function. errno is set on domain and range errors. */
#ifndef _MATH_H
#define _MATH_H

#ifdef _SINGLE_FP
#define __C166_D(name) __asm__("_" #name "f")
#else
#define __C166_D(name)
#endif

#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 0
#define math_errhandling MATH_ERRNO

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x)                                                          \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, \
                       (x))
#define isfinite(x) __builtin_isfinite(x)
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)

typedef float float_t;
typedef double double_t;

#define __C166_MATH1(name)                                                     \
  double name(double x) __C166_D(name);                                        \
  float name##f(float x);
#define __C166_MATH2(name)                                                     \
  double name(double x, double y) __C166_D(name);                              \
  float name##f(float x, float y);

__C166_MATH1(acos)
__C166_MATH1(asin)
__C166_MATH1(atan)
__C166_MATH2(atan2)
__C166_MATH1(cbrt)
__C166_MATH1(ceil)
__C166_MATH2(copysign)
__C166_MATH1(cos)
__C166_MATH1(cosh)
__C166_MATH1(exp)
__C166_MATH1(exp2)
__C166_MATH1(expm1)
__C166_MATH1(fabs)
__C166_MATH1(floor)
__C166_MATH2(fmax)
__C166_MATH2(fmin)
__C166_MATH2(fmod)
__C166_MATH2(hypot)
__C166_MATH1(log)
__C166_MATH1(log10)
__C166_MATH1(log1p)
__C166_MATH1(log2)
__C166_MATH1(nearbyint)
__C166_MATH2(pow)
__C166_MATH2(remainder)
__C166_MATH1(rint)
__C166_MATH1(round)
__C166_MATH1(sin)
__C166_MATH1(sinh)
__C166_MATH1(sqrt)
__C166_MATH1(tan)
__C166_MATH1(tanh)
__C166_MATH1(trunc)
double frexp(double x, int *exp) __C166_D(frexp);
float frexpf(float x, int *exp);
double ldexp(double x, int exp) __C166_D(ldexp);
float ldexpf(float x, int exp);
double scalbn(double x, int exp) __C166_D(scalbn);
float scalbnf(float x, int exp);
double modf(double x, double *ip) __C166_D(modf);
float modff(float x, float *ip);
long lround(double x) __C166_D(lround);
long lroundf(float x);

#undef __C166_MATH1
#undef __C166_MATH2

#endif
