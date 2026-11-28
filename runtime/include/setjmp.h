/* SPDX-License-Identifier: MIT */
/* <setjmp.h> of openc166's C library. A jmp_buf holds R0 (the user stack),
   SP (the system stack), R6-R9 and the return address (IP, and CSP with
   far code). */
#ifndef _SETJMP_H
#define _SETJMP_H

typedef int jmp_buf[8];

int setjmp(jmp_buf env);
_Noreturn void longjmp(jmp_buf env, int val);

#endif
