/* SPDX-License-Identifier: MIT */
#include <stdio.h>

/* Output in pieces of up to 32 bytes per _write. */
typedef struct {
  FILE *f;
  size_t n;
  char buf[32];
} out;

static void flush(out *o) {
  if (o->n && _write(o->f->fd, o->buf, o->n) != o->n)
    o->f->flags |= __C166_ERR;
  o->n = 0;
}

static void put(void *ctx, const char *s, size_t n) {
  out *o = ctx;
  while (n--) {
    if (o->n == sizeof o->buf)
      flush(o);
    o->buf[o->n++] = *s++;
  }
}

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap) {
  out o;
  o.f = f;
  o.n = 0;
  int n = __c166_format(put, &o, fmt, ap);
  flush(&o);
  return ferror(f) ? EOF : n;
}

int fprintf(FILE *restrict f, const char *restrict fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int vprintf(const char *restrict fmt, va_list ap) {
  return vfprintf(stdout, fmt, ap);
}

int printf(const char *restrict fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}
