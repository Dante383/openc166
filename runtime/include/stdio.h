/* SPDX-License-Identifier: MIT */
/* <stdio.h> of openc166's C library.

   There are no files: stdin, stdout and stderr are file descriptors 0, 1
   and 2, read and written through _read and _write, as in TASKING. The
   library's own _read and _write use ASC0, polled; a program that defines
   them itself gets its own. Output is not buffered beyond one call.

   printf and friends format integers, characters, strings and pointers
   with flags, width, precision and the length modifiers hh h l ll j z t;
   floating-point conversions are not supported yet. There is no scanf. */
#ifndef _STDIO_H
#define _STDIO_H

#define __need_size_t
#define __need_NULL
#include <stddef.h>
#include <stdarg.h>

typedef struct __c166_file {
  int fd;
  int unget;            /* a character from ungetc, or EOF */
  unsigned char flags;  /* __C166_EOF, __C166_ERR */
} FILE;
#define __C166_EOF 1
#define __C166_ERR 2

extern FILE __c166_iob[3];
#define stdin (&__c166_iob[0])
#define stdout (&__c166_iob[1])
#define stderr (&__c166_iob[2])

#define EOF (-1)
#define BUFSIZ 64

size_t _read(int fd, char *buffer, size_t size);
size_t _write(int fd, char *buffer, size_t count);

int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *restrict s, FILE *restrict f);
int puts(const char *s);
size_t fwrite(const void *restrict p, size_t size, size_t n,
              FILE *restrict f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *restrict s, int n, FILE *restrict f);
size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f);
int fflush(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
void perror(const char *s);

int printf(const char *restrict fmt, ...);
int fprintf(FILE *restrict f, const char *restrict fmt, ...);
int sprintf(char *restrict s, const char *restrict fmt, ...);
int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...);
int vprintf(const char *restrict fmt, va_list ap);
int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap);
int vsprintf(char *restrict s, const char *restrict fmt, va_list ap);
int vsnprintf(char *restrict s, size_t n, const char *restrict fmt,
              va_list ap);

/* The formatter behind them: hands the output to put in pieces. */
int __c166_format(void (*put)(void *ctx, const char *s, size_t n), void *ctx,
                  const char *fmt, va_list ap);

#endif
