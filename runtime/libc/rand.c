/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

static unsigned long next = 1;

/* The example in the C standard. */
int rand(void) {
  next = next * 1103515245 + 12345;
  return (unsigned)(next >> 16) & RAND_MAX;
}

void srand(unsigned seed) { next = seed; }
