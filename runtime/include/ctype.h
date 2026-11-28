/* SPDX-License-Identifier: MIT */
/* <ctype.h> of openc166's C library (the "C" locale, ASCII). */
#ifndef _CTYPE_H
#define _CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isascii(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int toascii(int c);
int tolower(int c);
int toupper(int c);
/* TASKING's: no check that c is a letter. */
#define _tolower(c) ((c) | 0x20)
#define _toupper(c) ((c) & ~0x20)

#endif
