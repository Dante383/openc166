/* SPDX-License-Identifier: MIT */
#include <stdio.h>

typedef struct {
  char *p;
  size_t left; /* room for characters, not counting the final 0 */
} out;

static void put(void *ctx, const char *s, size_t n) {
  out *o = ctx;
  for (; n && o->left; n--, o->left--)
    *o->p++ = *s++;
}

int vsnprintf(char *restrict s, size_t n, const char *restrict fmt,
              va_list ap) {
  out o;
  o.p = s;
  o.left = n ? n - 1 : 0;
  int len = __c166_format(put, &o, fmt, ap);
  if (n)
    *o.p = 0;
  return len;
}

int vsprintf(char *restrict s, const char *restrict fmt, va_list ap) {
  return vsnprintf(s, (size_t)-1, fmt, ap);
}

int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(s, n, fmt, ap);
  va_end(ap);
  return len;
}

int sprintf(char *restrict s, const char *restrict fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(s, (size_t)-1, fmt, ap);
  va_end(ap);
  return len;
}
