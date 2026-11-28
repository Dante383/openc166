/* SPDX-License-Identifier: MIT */
#include <string.h>

/* The "C" locale: the string itself. */
size_t strxfrm(char *restrict dst, const char *restrict src, size_t n) {
  size_t len = strlen(src);
  if (n) {
    size_t k = len < n ? len + 1 : n;
    memcpy(dst, src, k);
  }
  return len;
}
