/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

div_t div(int num, int den) {
  div_t r = {num / den, num % den};
  return r;
}
ldiv_t ldiv(long num, long den) {
  ldiv_t r = {num / den, num % den};
  return r;
}
lldiv_t lldiv(long long num, long long den) {
  lldiv_t r = {num / den, num % den};
  return r;
}
