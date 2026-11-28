/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* Converts like strtoul with an unsigned long accumulator; *neg says
   whether there was a minus sign. ERANGE and ULONG_MAX on overflow. */
static unsigned long convert(const char *s, char **end, int base, int *neg,
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
  unsigned long v = 0, limit = ULONG_MAX / base;
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
    if (v > limit || (v == limit && (unsigned)d > ULONG_MAX % base))
      *overflow = 1;
    v = v * base + d;
  }
  if (end)
    *end = (char *)(p == digits ? s : p);
  return v;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base) {
  int neg, overflow;
  unsigned long v = convert(s, end, base, &neg, &overflow);
  if (overflow) {
    errno = ERANGE;
    return ULONG_MAX;
  }
  return neg ? -v : v;
}

long strtol(const char *restrict s, char **restrict end, int base) {
  int neg, overflow;
  unsigned long v = convert(s, end, base, &neg, &overflow);
  if (overflow || v > (unsigned long)LONG_MAX + neg) {
    errno = ERANGE;
    return neg ? LONG_MIN : LONG_MAX;
  }
  return neg ? -(long)(v - 1) - 1 : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
