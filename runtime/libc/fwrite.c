/* SPDX-License-Identifier: MIT */
#include <stdio.h>

size_t fwrite(const void *restrict p, size_t size, size_t n,
              FILE *restrict f) {
  if (!size || !n)
    return 0;
  size_t done = _write(f->fd, (char *)p, size * n);
  if (done != size * n)
    f->flags |= __C166_ERR;
  return done / size;
}
