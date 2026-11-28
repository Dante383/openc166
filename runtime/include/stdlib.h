/* SPDX-License-Identifier: MIT */
/* <stdlib.h> of openc166's C library. */
#ifndef _STDLIB_H
#define _STDLIB_H

#define __need_size_t
#define __need_wchar_t
#define __need_NULL
#include <stddef.h>

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#define MB_CUR_MAX 1

int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base);
double strtod(const char *restrict s, char **restrict end);

int rand(void);
void srand(unsigned seed);

/* The heap is __c166_heap, __c166_heap_size bytes (1024 by default; a
   program that defines both names itself gets its own). */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t size);
void free(void *p);

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

/* exit returns from __cstart to its caller with the status in R4, as
   returning from main does. */
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
_Noreturn void _exit(int status);
_Noreturn void abort(void);
int atexit(void (*fn)(void));
char *getenv(const char *name);

#endif
