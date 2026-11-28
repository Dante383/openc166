/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

void abort(void) { _exit(EXIT_FAILURE); }

void _Exit(int status) { _exit(status); }
