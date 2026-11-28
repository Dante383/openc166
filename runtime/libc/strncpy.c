/* SPDX-License-Identifier: MIT */
#include <string.h>

char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
  char *d = dst;
  for (; n && *src; n--)
    *d++ = *src++;
  for (; n; n--)
    *d++ = 0;
  return dst;
}
