/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert(const char *expr, const char *file, int line) {
  char num[7], *p = num + sizeof num;
  unsigned n = line;
  *--p = 0;
  do
    *--p = '0' + n % 10;
  while (n /= 10);
  fputs("Assertion failed: ", stderr);
  fputs(expr, stderr);
  fputs(", file ", stderr);
  fputs(file, stderr);
  fputs(", line ", stderr);
  fputs(p, stderr);
  fputc('\n', stderr);
  abort();
}
