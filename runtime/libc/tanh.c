/* SPDX-License-Identifier: MIT */
#include <math.h>

double tanh(double x) {
  double a = fabs(x);
  double r;
  if (a > 22)
    r = 1; /* tanh(22) is 1 to double precision */
  else {
    /* (e^2a - 1) / (e^2a + 1) = t / (t + 2) with t = e^2a - 1 */
    double t = expm1(2 * a);
    r = t / (t + 2);
  }
  return x < 0 ? -r : r;
}
