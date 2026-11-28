/* SPDX-License-Identifier: MIT */
#include <math.h>

double cosh(double x) {
  double a = fabs(x);
  if (a < 22) {
    double e = exp(a);
    return 0.5 * (e + 1 / e);
  }
  return exp(a - 0.69314718055994530942);
}
