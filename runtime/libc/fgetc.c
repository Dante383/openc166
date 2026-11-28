/* SPDX-License-Identifier: MIT */
#include <stdio.h>

int fgetc(FILE *f) {
  if (f->unget != EOF) {
    int c = f->unget;
    f->unget = EOF;
    return c;
  }
  char c;
  if (_read(f->fd, &c, 1) != 1) {
    f->flags |= __C166_EOF;
    return EOF;
  }
  return (unsigned char)c;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
  if (c == EOF || f->unget != EOF)
    return EOF;
  f->unget = (unsigned char)c;
  f->flags &= ~__C166_EOF;
  return f->unget;
}
