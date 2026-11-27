/* 32-bit unsigned division and remainder, by shift and subtract.
 * Only shifts by constants: a variable 32-bit shift would call
 * __ashlsi3 and friends. */

__attribute__((noinline)) static unsigned long divmod(unsigned long n,
                                                    unsigned long d,
                                                    unsigned long *rem) {
  unsigned long q = 0, r = 0;
  for (int i = 0; i < 32; i++) {
    r = (r << 1) | (n >> 31);
    n <<= 1;
    q <<= 1;
    if (r >= d) {
      r -= d;
      q |= 1;
    }
  }
  *rem = r;
  return q;
}

unsigned long __udivsi3(unsigned long n, unsigned long d) {
  unsigned long r;
  return divmod(n, d, &r);
}

unsigned long __umodsi3(unsigned long n, unsigned long d) {
  unsigned long r;
  divmod(n, d, &r);
  return r;
}

long __divsi3(long n, long d) {
  int neg = (n < 0) != (d < 0);
  unsigned long r;
  unsigned long q = divmod(n < 0 ? -(unsigned long)n : (unsigned long)n,
                           d < 0 ? -(unsigned long)d : (unsigned long)d, &r);
  return neg ? -(long)q : (long)q;
}

long __modsi3(long n, long d) {
  unsigned long r;
  divmod(n < 0 ? -(unsigned long)n : (unsigned long)n,
         d < 0 ? -(unsigned long)d : (unsigned long)d, &r);
  return n < 0 ? -(long)r : (long)r;
}
