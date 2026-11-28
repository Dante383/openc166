/* SPDX-License-Identifier: MIT */
#include <string.h>

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  for (; s[n]; n++) {
    const char *a = accept;
    while (*a && *a != s[n])
      a++;
    if (!*a)
      break;
  }
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  for (; s[n]; n++)
    for (const char *r = reject; *r; r++)
      if (*r == s[n])
        return n;
  return n;
}

char *strpbrk(const char *s, const char *accept) {
  s += strcspn(s, accept);
  return *s ? (char *)s : NULL;
}
