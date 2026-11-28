/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

/* There is no environment. */
char *getenv(const char *name) {
  (void)name;
  return NULL;
}
