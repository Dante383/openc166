/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <stdlib.h>

/* Decimal only, digit by digit: not correctly rounded in the last bits. */
double strtod(const char *restrict s, char **restrict end) {
  const char *p = s;
  while (isspace(*p))
    p++;
  int neg = 0;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  double v = 0;
  int digits = 0, exp = 0;
  for (; isdigit(*p); p++, digits++)
    v = v * 10 + (*p - '0');
  if (*p == '.')
    for (p++; isdigit(*p); p++, digits++, exp--)
      v = v * 10 + (*p - '0');
  if (!digits) {
    if (end)
      *end = (char *)s;
    return 0;
  }
  if ((*p | 32) == 'e') {
    const char *q = p + 1;
    int eneg = 0, e = 0;
    if (*q == '+' || *q == '-')
      eneg = *q++ == '-';
    if (isdigit(*q)) {
      for (; isdigit(*q); q++)
        if (e < 10000)
          e = e * 10 + (*q - '0');
      exp += eneg ? -e : e;
      p = q;
    }
  }
  double scale = 10;
  unsigned n = exp < 0 ? -exp : exp;
  double f = 1;
  for (; n; n >>= 1, scale *= scale)
    if (n & 1)
      f *= scale;
  v = exp < 0 ? v / f : v * f;
  if (end)
    *end = (char *)p;
  return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, NULL); }
