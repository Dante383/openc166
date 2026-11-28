/* SPDX-License-Identifier: MIT */
#include <stdio.h>

size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f) {
  char *d = p;
  size_t total = size * n, done = 0;
  for (; done < total; done++) {
    int c = fgetc(f);
    if (c == EOF)
      break;
    d[done] = (char)c;
  }
  return size ? done / size : 0;
}
