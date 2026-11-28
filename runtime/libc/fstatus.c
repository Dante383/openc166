/* SPDX-License-Identifier: MIT */
#include <stdio.h>

/* Output is not buffered: nothing to flush. */
int fflush(FILE *f) {
  (void)f;
  return 0;
}
int feof(FILE *f) { return f->flags & __C166_EOF; }
int ferror(FILE *f) { return f->flags & __C166_ERR; }
void clearerr(FILE *f) { f->flags = 0; }
