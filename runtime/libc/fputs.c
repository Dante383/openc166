/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

int fputs(const char *restrict s, FILE *restrict f) {
  size_t n = strlen(s);
  if (_write(f->fd, (char *)s, n) != n) {
    f->flags |= __C166_ERR;
    return EOF;
  }
  return 0;
}

int puts(const char *s) {
  if (fputs(s, stdout) == EOF)
    return EOF;
  return fputc('\n', stdout) == EOF ? EOF : 0;
}
