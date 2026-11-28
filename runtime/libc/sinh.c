/* SPDX-License-Identifier: MIT */
#include <math.h>

/* LLVM libc has sinh, cosh and tanh for float only; the double ones are
   built on expm1 and exp. */
double sinh(double x) {
  double a = fabs(x);
  if (a < 22) {
    /* (e^a - e^-a) / 2 = (t + t / (t + 1)) / 2 with t = e^a - 1 */
    double t = expm1(a);
    double r = 0.5 * (t + t / (t + 1));
    return x < 0 ? -r : r;
  }
  /* e^a / 2, without overflowing early */
  double r = exp(a - 0.69314718055994530942);
  return x < 0 ? -r : r;
}
