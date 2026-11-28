/* SPDX-License-Identifier: MIT */
#include <reg167.h>
#include <stdio.h>

/* The default _write: every file descriptor goes to ASC0, polled. The
   program sets up ASC0 (S0BG, S0CON, the port pins) first, or defines its
   own _write. */
size_t _write(int fd, char *buffer, size_t count) {
  (void)fd;
  for (size_t i = 0; i < count; i++) {
    S0TIR = 0;
    S0TBUF = (unsigned char)buffer[i];
    while (!S0TIR)
      ;
  }
  return count;
}
