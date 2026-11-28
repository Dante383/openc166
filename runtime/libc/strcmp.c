/* SPDX-License-Identifier: MIT */
#include <string.h>

int strcmp(const char *a, const char *b) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  while (*p && *p == *q)
    p++, q++;
  return *p - *q;
}

/* The "C" locale collates as strcmp. */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
