/* SPDX-License-Identifier: MIT */
#include <ctype.h>

/* The "C" locale, ASCII. */
int isascii(int c) { return (unsigned)c < 128; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (unsigned)c < 32 || c == 127; }
int isdigit(int c) { return (unsigned)(c - '0') < 10; }
int islower(int c) { return (unsigned)(c - 'a') < 26; }
int isupper(int c) { return (unsigned)(c - 'A') < 26; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isgraph(int c) { return (unsigned)(c - 33) < 94; }
int isprint(int c) { return (unsigned)(c - 32) < 95; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isspace(int c) { return c == ' ' || (unsigned)(c - '\t') < 5; }
int isxdigit(int c) { return isdigit(c) || (unsigned)((c | 32) - 'a') < 6; }
int toascii(int c) { return c & 127; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
