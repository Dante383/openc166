/* SPDX-License-Identifier: MIT */
#include <stdio.h>

char *fgets(char *restrict s, int n, FILE *restrict f) {
  char *p = s;
  if (n <= 0)
    return NULL;
  while (--n) {
    int c = fgetc(f);
    if (c == EOF) {
      if (p == s)
        return NULL;
      break;
    }
    *p++ = (char)c;
    if (c == '\n')
      break;
  }
  *p = 0;
  return s;
}
