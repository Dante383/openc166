/* SPDX-License-Identifier: MIT */
#include <string.h>

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  for (; *haystack; haystack++) {
    const char *h = haystack, *n = needle;
    while (*n && *h == *n)
      h++, n++;
    if (!*n)
      return (char *)haystack;
  }
  return NULL;
}
