/* SPDX-License-Identifier: MIT */
#include <string.h>

char *strtok(char *restrict s, const char *restrict delim) {
  static char *next;
  if (!s)
    s = next;
  if (!s)
    return NULL;
  s += strspn(s, delim);
  if (!*s) {
    next = NULL;
    return NULL;
  }
  char *end = s + strcspn(s, delim);
  if (*end)
    *end++ = 0;
  else
    end = NULL;
  next = end;
  return s;
}
