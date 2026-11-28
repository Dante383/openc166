/* SPDX-License-Identifier: MIT */
#include <reg167.h>
#include <stdio.h>

/* The default _read: from ASC0, polled, waiting for each character. */
size_t _read(int fd, char *buffer, size_t size) {
  (void)fd;
  for (size_t i = 0; i < size; i++) {
    while (!S0RIR)
      ;
    S0RIR = 0;
    buffer[i] = (char)S0RBUF;
  }
  return size;
}
