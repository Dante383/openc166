/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

static void swap(char *a, char *b, size_t size) {
  while (size--) {
    char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

/* Shell sort: no recursion and little code; not stable, as qsort need
   not be. */
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
  char *b = base;
  size_t gap = 1;
  while (gap < n / 3)
    gap = 3 * gap + 1;
  for (; gap; gap /= 3)
    for (size_t i = gap; i < n; i++)
      for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0;
           j -= gap)
        swap(b + (j - gap) * size, b + j * size, size);
}
