/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* printf's formatter: %d %i %u %o %x %X %c %s %p %n %%, the flags - + space
   # 0, width and precision (numbers or *), the lengths hh h l ll j z t.
   Floating-point conversions are not supported yet: they take their
   argument and print nothing. */

enum { LEFT = 1, PLUS = 2, SPACE = 4, ALT = 8, ZERO = 16 };

typedef void put_fn(void *ctx, const char *s, size_t n);

static void pad(put_fn *put, void *ctx, char c, int n) {
  static const char spaces[8] = "        ", zeros[8] = "00000000";
  const char *s = c == '0' ? zeros : spaces;
  for (; n > 0; n -= 8)
    put(ctx, s, n < 8 ? n : 8);
}

int __c166_format(put_fn *put, void *ctx, const char *fmt, va_list ap) {
  int total = 0;
  while (*fmt) {
    if (*fmt != '%') {
      const char *start = fmt;
      while (*fmt && *fmt != '%')
        fmt++;
      put(ctx, start, fmt - start);
      total += fmt - start;
      continue;
    }
    fmt++;
    int flags = 0;
    for (;; fmt++) {
      if (*fmt == '-')
        flags |= LEFT;
      else if (*fmt == '+')
        flags |= PLUS;
      else if (*fmt == ' ')
        flags |= SPACE;
      else if (*fmt == '#')
        flags |= ALT;
      else if (*fmt == '0')
        flags |= ZERO;
      else
        break;
    }
    int width = 0, prec = -1;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        flags |= LEFT;
        width = -width;
      }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9')
          prec = prec * 10 + (*fmt++ - '0');
      }
    }
    /* Length: 0 int, 1 long, 2 long long, -1 short, -2 char. */
    int len = 0;
    for (;; fmt++) {
      if (*fmt == 'l')
        len++;
      else if (*fmt == 'h')
        len--;
      else if (*fmt == 'j')
        len = 2;
      else if (*fmt == 'z' || *fmt == 't')
        len = sizeof(size_t) == sizeof(long) ? 1 : 0;
      else
        break;
    }
    char conv = *fmt;
    if (!conv)
      break;
    fmt++;

    char buf[24], *end = buf + sizeof buf, *p = end;
    const char *prefix = "";
    const char *s;
    int n;
    switch (conv) {
    case '%':
      put(ctx, "%", 1);
      total++;
      continue;
    case 'c':
      buf[0] = (char)va_arg(ap, int);
      s = buf;
      n = 1;
      prec = -1;
      break;
    case 's':
      s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      for (n = 0; (prec < 0 || n < prec) && s[n]; n++)
        ;
      prec = -1;
      break;
    case 'n':
      if (len >= 2)
        *va_arg(ap, long long *) = total;
      else if (len == 1)
        *va_arg(ap, long *) = total;
      else if (len == -2)
        *va_arg(ap, signed char *) = total;
      else if (len == -1)
        *va_arg(ap, short *) = total;
      else
        *va_arg(ap, int *) = total;
      continue;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a':
    case 'A':
      (void)va_arg(ap, double);
      continue;
    default: {
      unsigned base = 10;
      int is_signed = 0, upper = 0;
      unsigned long long v;
      if (conv == 'd' || conv == 'i')
        is_signed = 1;
      else if (conv == 'o')
        base = 8;
      else if (conv == 'x' || conv == 'X' || conv == 'p') {
        base = 16;
        upper = conv == 'X';
      } else if (conv != 'u') {
        continue; /* unknown */
      }
      if (conv == 'p') {
        v = (uintptr_t)va_arg(ap, void *);
        flags |= ALT;
        if (prec < 0)
          prec = 2 * sizeof(void *);
      } else if (len >= 2) {
        v = va_arg(ap, unsigned long long);
      } else if (len == 1) {
        v = is_signed ? (unsigned long long)va_arg(ap, long)
                      : va_arg(ap, unsigned long);
      } else {
        unsigned u = va_arg(ap, unsigned);
        if (len == -1)
          u = is_signed ? (unsigned)(short)u : (unsigned short)u;
        else if (len <= -2)
          u = is_signed ? (unsigned)(signed char)u : (unsigned char)u;
        v = is_signed ? (unsigned long long)(int)u : u;
      }
      if (is_signed) {
        if ((long long)v < 0) {
          v = -v;
          prefix = "-";
        } else if (flags & PLUS) {
          prefix = "+";
        } else if (flags & SPACE) {
          prefix = " ";
        }
      }
      const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
      if (v >> 32 == 0) {
        /* 32-bit division where it is enough. */
        unsigned long w = (unsigned long)v;
        while (w) {
          *--p = digits[w % base];
          w /= base;
        }
      } else {
        while (v) {
          *--p = digits[v % base];
          v /= base;
        }
      }
      int ndigits = end - p;
      if (flags & ALT) {
        if (base == 16 && (ndigits || conv == 'p'))
          prefix = upper ? "0X" : "0x";
        else if (base == 8 && (prec <= ndigits))
          prec = ndigits + 1;
      }
      if (prec < 0)
        prec = 1;
      else
        flags &= ~ZERO;
      if (prec > ndigits) {
        int zeros = prec - ndigits;
        if (zeros > p - buf)
          zeros = p - buf;
        while (zeros--)
          *--p = '0';
      }
      s = p;
      n = end - p;
      prec = -1;
      break;
    }
    }
    int plen = strlen(prefix);
    int fill = width - n - plen;
    if (!(flags & (LEFT | ZERO)))
      pad(put, ctx, ' ', fill);
    put(ctx, prefix, plen);
    if ((flags & (LEFT | ZERO)) == ZERO)
      pad(put, ctx, '0', fill);
    put(ctx, s, n);
    if (flags & LEFT)
      pad(put, ctx, ' ', fill);
    total += n + plen + (fill > 0 ? fill : 0);
  }
  return total;
}
