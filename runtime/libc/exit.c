/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#define MAX_ATEXIT 8

static void (*handlers[MAX_ATEXIT])(void);
static int count;

int atexit(void (*fn)(void)) {
  if (count == MAX_ATEXIT)
    return -1;
  handlers[count++] = fn;
  return 0;
}

void exit(int status) {
  while (count)
    handlers[--count]();
  _exit(status);
}
