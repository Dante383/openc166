/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

char *strerror(int errnum) {
  switch (errnum) {
  case 0:
    return (char *)"no error";
  case EDOM:
    return (char *)"argument out of domain";
  case ERANGE:
    return (char *)"result out of range";
  case EILSEQ:
    return (char *)"illegal byte sequence";
  default:
    return (char *)"unknown error";
  }
}
