/* 64-bit division and remainder, by shift and subtract (constant shifts
 * only, which the compiler does inline). */

typedef unsigned long long u64;

__attribute__((noinline)) static u64 divmod64(u64 n, u64 d, u64 *rem) {
  u64 q = 0, r = 0;
  for (int i = 0; i < 64; i++) {
    r = (r << 1) | (n >> 63);
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

u64 __udivdi3(u64 n, u64 d) {
  u64 r;
  return divmod64(n, d, &r);
}

u64 __umoddi3(u64 n, u64 d) {
  u64 r;
  divmod64(n, d, &r);
  return r;
}

long long __divdi3(long long n, long long d) {
  int neg = (n < 0) != (d < 0);
  u64 r;
  u64 q = divmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
  return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long n, long long d) {
  u64 r;
  divmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
  return n < 0 ? -(long long)r : (long long)r;
}
