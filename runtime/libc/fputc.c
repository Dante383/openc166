/* SPDX-License-Identifier: MIT */
#include <stdio.h>

int fputc(int c, FILE *f) {
  char ch = (char)c;
  if (_write(f->fd, &ch, 1) != 1) {
    f->flags |= __C166_ERR;
    return EOF;
  }
  return (unsigned char)ch;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
