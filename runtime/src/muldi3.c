/* 64-bit multiplication from 16 x 16 -> 32-bit products (MULU). */

typedef unsigned long long u64;

long long __muldi3(long long a, long long b) {
  unsigned int x[4], y[4], r[4] = {0, 0, 0, 0};
  u64 ua = (u64)a, ub = (u64)b;
  for (int i = 0; i < 4; i++) {
    x[i] = (unsigned int)(ua >> (16 * i));
    y[i] = (unsigned int)(ub >> (16 * i));
  }
  for (int i = 0; i < 4; i++) {
    unsigned long carry = 0;
    for (int j = 0; i + j < 4; j++) {
      unsigned long t = (unsigned long)x[i] * y[j] + r[i + j] + carry;
      r[i + j] = (unsigned int)t;
      carry = t >> 16;
    }
  }
  return (long long)(((u64)r[3] << 48) | ((u64)r[2] << 32) |
                     ((u64)r[1] << 16) | r[0]);
}
