/* SPDX-License-Identifier: MIT */
#include <string.h>

int strncmp(const char *a, const char *b, size_t n) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  for (; n; n--, p++, q++) {
    if (*p != *q)
      return *p - *q;
    if (!*p)
      break;
  }
  return 0;
}
