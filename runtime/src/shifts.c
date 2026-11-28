/* 32-bit shifts by a variable amount, built from 16-bit shifts. */

static unsigned long join(unsigned hi, unsigned lo) {
  return ((unsigned long)hi << 16) | lo;
}

long __ashlsi3(long a, int b) {
  unsigned lo = (unsigned)a, hi = (unsigned)((unsigned long)a >> 16);
  b &= 31;
  if (b >= 16) {
    hi = lo << (b - 16);
    lo = 0;
  } else if (b) {
    hi = (hi << b) | (lo >> (16 - b));
    lo <<= b;
  }
  return (long)join(hi, lo);
}

unsigned long __lshrsi3(unsigned long a, int b) {
  unsigned lo = (unsigned)a, hi = (unsigned)(a >> 16);
  b &= 31;
  if (b >= 16) {
    lo = hi >> (b - 16);
    hi = 0;
  } else if (b) {
    lo = (lo >> b) | (hi << (16 - b));
    hi >>= b;
  }
  return join(hi, lo);
}

long __ashrsi3(long a, int b) {
  unsigned lo = (unsigned)a;
  int hi = (int)(a >> 16);
  b &= 31;
  if (b >= 16) {
    lo = (unsigned)(hi >> (b - 16));
    hi = hi < 0 ? -1 : 0;
  } else if (b) {
    lo = (lo >> b) | ((unsigned)hi << (16 - b));
    hi >>= b;
  }
  return (long)join((unsigned)hi, lo);
}
