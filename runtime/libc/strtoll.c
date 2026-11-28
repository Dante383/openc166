/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* Converts like strtoul with an unsigned long long accumulator; *neg says
   whether there was a minus sign. ERANGE and ULLONG_MAX on overflow. */
static unsigned long long convert(const char *s, char **end, int base, int *neg,
                             int *overflow) {
  const char *p = s;
  while (isspace(*p))
    p++;
  *neg = 0;
  if (*p == '+' || *p == '-')
    *neg = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' &&
      isxdigit(p[2])) {
    p += 2;
    base = 16;
  } else if (base == 0) {
    base = *p == '0' ? 8 : 10;
  }
  unsigned long long v = 0, limit = ULLONG_MAX / base;
  const char *digits = p;
  *overflow = 0;
  for (;; p++) {
    int d;
    if (isdigit(*p))
      d = *p - '0';
    else if (isalpha(*p))
      d = (*p | 32) - 'a' + 10;
    else
      break;
    if (d >= base)
      break;
    if (v > limit || (v == limit && (unsigned)d > ULLONG_MAX % base))
      *overflow = 1;
    v = v * base + d;
  }
  if (end)
    *end = (char *)(p == digits ? s : p);
  return v;
}

unsigned long long strtoull(const char *restrict s, char **restrict end, int base) {
  int neg, overflow;
  unsigned long long v = convert(s, end, base, &neg, &overflow);
  if (overflow) {
    errno = ERANGE;
    return ULLONG_MAX;
  }
  return neg ? -v : v;
}

long long strtoll(const char *restrict s, char **restrict end, int base) {
  int neg, overflow;
  unsigned long long v = convert(s, end, base, &neg, &overflow);
  if (overflow || v > (unsigned long long)LLONG_MAX + neg) {
    errno = ERANGE;
    return neg ? LLONG_MIN : LLONG_MAX;
  }
  return neg ? -(long long)(v - 1) - 1 : (long long)v;
}

long long atoll(const char *s) { return strtoll(s, NULL, 10); }
