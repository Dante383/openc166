# SPDX-License-Identifier: MIT
"""C compiled by the C166 LLVM back end, run in the simulator.

Needs clang from the openc166-llvm fork: the OPENC166_CLANG environment
variable, or ../openc166-llvm/build/bin/clang next to this repository.
The tests are skipped when it is not there.

Each test compiles C to A166 assembly, links it with openc166 and calls
functions in the simulator with the TASKING calling convention: arguments
in R12-R15 (then the user stack), the result in R4 (R4:R5 for 32 bits),
the user stack in R0, the return address on the system stack.
"""

import math
import os
import struct
import subprocess
import tempfile
import unittest

import openc166
from openc166 import sim

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNTIME = os.path.join(ROOT, "runtime")


def _find_clang():
    path = os.environ.get("OPENC166_CLANG")
    if path:
        return path
    path = os.path.join(os.path.dirname(ROOT), "openc166-llvm", "build", "bin", "clang")
    return path if os.path.exists(path) else None


CLANG = _find_clang()

# Code and constants in the ROM image; data, the user stack and the system
# stack in the C167's internal RAM (0F600H..0FDFFH, register bank at 0FC00H).
ROM = (0x1000, 0x7FFF)
RAM = (0xF600, 0xF9FF)
USER_STACK = 0xFB00
HALT = 0x0100

HARNESS = """$CASE
HALT_PR SECTION CODE AT 0100H
        PUBLIC  __halt
__halt: NOP
        JMP     cc_UC, __halt
HALT_PR ENDS
        END
"""


def compile_c(source, opt="-O2", name="test.c"):
    with tempfile.TemporaryDirectory() as tmp:
        c_path = os.path.join(tmp, name)
        with open(c_path, "w") as f:
            f.write(source)
        out = subprocess.run(
            [CLANG, "--target=c166", *opt.split(), "-S", "-o", "-", name],
            capture_output=True,
            text=True,
            cwd=tmp,
            # The C library's headers, from this tree's runtime.
            env={**os.environ, "OPENC166_RUNTIME": RUNTIME},
        )
        if out.returncode != 0:
            raise AssertionError("clang failed:\n" + out.stderr)
        return out.stdout


class Program:
    """C modules linked with openc166 and loaded into a simulated C167."""

    def __init__(self, sources, opt="-O2", startup=False, classes=None, image=None, board="c167",
                 sections=None, tiny=False, model="small"):
        if tiny:
            model = "tiny"
        # The harness module matches the model: $SEGMENTED but in tiny.
        harness = HARNESS if model == "tiny" else HARNESS.replace("$CASE\n", "$CASE\n$SEGMENTED\n", 1)
        modules = [("harness.a66", harness)]
        # The runtime of the model (as the driver picks it): <model>/lib and
        # <model>/cstart.a66, or small's.
        runtime = RUNTIME if model == "small" else os.path.join(RUNTIME, model)
        if startup:
            start = os.path.join(runtime, "cstart.a66")
            if not os.path.exists(start):
                start = os.path.join(RUNTIME, "cstart.a66")
            with open(start) as f:
                modules.append(("cstart.a66", f.read()))
        self.tiny = model == "tiny"
        self.far_code = model in ("small", "large")
        if model != "small":
            opt += " -mabi=" + model
        for i, src in enumerate(sources):
            modules.append((f"m{i}.a66", compile_c(src, opt, name=f"m{i}.c")))
        self.asm = modules
        self.board = board
        self.result = openc166.assemble_many(
            modules,
            classes=classes or {"CPROGRAM": [ROM], "CNEARROM": [ROM], "CNEAR": [RAM]},
            sections=sections,
            libraries=[os.path.join(runtime, "lib")],
        )
        if not self.result.ok:
            listing = "\n".join(str(e) for e in self.result.errors)
            raise AssertionError("openc166 failed:\n" + listing + "\n" + modules[-1][1])
        # Without an image, the program is the ROM. With one (a flash image),
        # what lies in the internal RAM is loaded as a loader would, the
        # rest goes into the image as if flashed.
        rom = bytearray(image if image is not None else b"\xff" * 0x8000)
        self.loaded = []
        for chunk in self.result.chunks:
            if image is not None and 0xF000 <= chunk.address < 0x10000:
                self.loaded.append(chunk)
            else:
                assert chunk.end <= len(rom), "outside the image"
                rom[chunk.address : chunk.end] = chunk.data
        self.rom = bytes(rom)
        self.sections = self.result.sections

    def simulator(self):
        s = sim.Simulator(self.rom, board=self.board)
        for chunk in self.loaded:
            s.write(chunk.address, chunk.data)
        return s

    def address(self, name):
        return self.result.symbol(name).value

    def init_variables(self, s):
        """What cstart.a66 does before main: initial values from ROM to RAM,
        zero for the rest."""
        data = self.sections.get("C166_DATA")
        init = self.sections.get("C166_DATA_INIT")
        if data and data.size:
            s.write(data.address, self.rom[init.address : init.address + data.size])
        for name in ("C166_BSS", "C166_BITWORD"):
            sec = self.sections.get(name)
            if sec and sec.size:
                s.write(sec.address, bytes(sec.size))

    def call(self, name, *args, max_instructions=200000):
        """Calls a C function with 16-bit arguments, placed as TASKING
        places them (see place()); returns R4, R5 and the simulator."""
        regs, stack = place([[a] for a in args])
        return self.call_placed(name, regs, stack, max_instructions)

    def call_placed(self, name, regs, stack, max_instructions=200000):
        s = self.simulator()
        self.init_variables(s)
        for reg, v in zip((12, 13, 14, 15), regs):
            s.r[reg] = v & 0xFFFF
        r0 = USER_STACK - 2 * len(stack)
        for i, v in enumerate(stack):
            s.write16(r0 + 2 * i, v & 0xFFFF)
        s.r[0] = r0
        push_return(s, far=self.far_code)
        s.break_at(HALT)
        s.pc = self.address(name if name.startswith("_") else "_" + name)
        stop = s.cont(max_instructions)
        if s.pc != HALT:
            raise AssertionError(f"{name} did not return: {stop}, pc={s.pc:06X}")
        if s.r[0] != r0:
            raise AssertionError(f"{name} left R0 at {s.r[0]:04X}, not {r0:04X}")
        return s.r[4], s.r[5], s


def push_return(s, far=True):
    """The return address a CALLS (far, the small model) or CALLA (near,
    the tiny model) would leave on the system stack: back to HALT."""
    if far:
        sp = s.sfr["SP"] - 4
        s.write16(sp, HALT)  # IP, popped first by RETS
        s.write16(sp + 2, 0)  # CSP
    else:
        sp = s.sfr["SP"] - 2
        s.write16(sp, HALT)
    s.sfr["SP"] = sp


def place(args):
    """TASKING C166 Classic: arguments (lists of words, low word first) go
    in R12-R15 while they fit; one that does not fit goes on the stack, and
    so does everything after it."""
    regs, stack = [], []
    for words in args:
        if stack or len(regs) + len(words) > 4:
            stack += words
        else:
            regs += words
    return regs, stack


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


def signed32(v):
    return v - 0x100000000 if v & 0x80000000 else v


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class CompiledCodeTest(unittest.TestCase):
    """The same programs at -O0 (everything through the stack) and -O2."""

    SOURCE = r"""
int g;
int table[5] = {3, 1, 4, 1, 5};
unsigned char bytes[4] = {0x80, 0x7f, 0x01, 0xff};
static int counter;

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int max(int a, int b) { return a > b ? a : b; }
unsigned umax(unsigned a, unsigned b) { return a > b ? a : b; }
int five(int a, int b, int c, int d, int e) { return a * 10000 + e; }
int six(int a, int b, int c, int d, int e, int f) { return e - f; }
int call_five(void) { return five(1, 2, 3, 4, 5) + six(0, 0, 0, 0, 9, 4); }
int sum_table(void) { int s = 0; for (int i = 0; i < 5; i++) s += table[i]; return s; }
int byte_u(int i) { return bytes[i]; }
int byte_s(int i) { return (signed char)bytes[i]; }
void set_byte(int i, int v) { bytes[i] = v; }
int bump(void) { g += 2; return ++counter; }
int mul(int a, int b) { return a * b; }
int sdiv(int a, int b) { return a / b; }
int smod(int a, int b) { return a % b; }
unsigned udiv(unsigned a, unsigned b) { return a / b; }
unsigned umod(unsigned a, unsigned b) { return a % b; }
int shl(int a, int n) { return a << n; }
int sar(int a, int n) { return a >> n; }
unsigned shr(unsigned a, int n) { return a >> n; }
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int local_array(int n) {
  int a[8];
  for (int i = 0; i < 8; i++) a[i] = i * n;
  int s = 0;
  for (int i = 0; i < 8; i++) s += a[i];
  return s;
}
int classify(int x) {
  switch (x) {
  case 0: return 100; case 1: return 200; case 2: return 300;
  case 3: return 400; case 4: return 500; case 5: return 600;
  default: return -1;
  }
}
int pressure(int a, int b, int c, int d) {
  int x = mul(a, b), y = mul(c, d), z = mul(x, y);
  return x + y + z + a + b + c + d;
}
"""

    def check(self, opt):
        p = Program([self.SOURCE], opt)
        r = lambda name, *args: p.call(name, *args)[0]
        self.assertEqual(r("add", 3, 4), 7)
        self.assertEqual(r("add", 0xFFFF, 1), 0)
        self.assertEqual(signed16(r("sub", 3, 5)), -2)
        self.assertEqual(signed16(r("max", -5, 3)), 3)
        self.assertEqual(signed16(r("max", 7, -3)), 7)
        self.assertEqual(r("umax", 0xFFFF, 3), 0xFFFF)
        self.assertEqual(r("five", 1, 2, 3, 4, 5), 10005)
        self.assertEqual(r("six", 0, 0, 0, 0, 9, 4), 5)
        self.assertEqual(r("call_five"), 10010)
        self.assertEqual(r("sum_table"), 14)
        self.assertEqual(r("byte_u", 0), 0x80)
        self.assertEqual(signed16(r("byte_s", 0)), -128)
        self.assertEqual(r("byte_s", 1), 0x7F)
        _, _, s = p.call("set_byte", 2, 0x1234)
        self.assertEqual(s.read(p.address("_bytes") + 2, 2), b"\x34\xff")
        _, _, s = p.call("bump")
        self.assertEqual(s.read16(p.address("_g")), 2)
        self.assertEqual(r("mul", 300, 7), 2100)
        self.assertEqual(signed16(r("mul", -3, 7)), -21)
        self.assertEqual(signed16(r("sdiv", -100, 7)), -14)
        self.assertEqual(signed16(r("smod", -100, 7)), -2)
        self.assertEqual(r("udiv", 60000, 7), 8571)
        self.assertEqual(r("umod", 60000, 7), 3)
        self.assertEqual(r("shl", 3, 4), 48)
        self.assertEqual(signed16(r("sar", -64, 3)), -8)
        self.assertEqual(r("shr", 0x8000, 15), 1)
        self.assertEqual(r("fib", 10), 55)
        self.assertEqual(r("local_array", 3), 84)
        self.assertEqual(r("classify", 4), 500)
        self.assertEqual(signed16(r("classify", 9)), -1)
        self.assertEqual(r("pressure", 1, 2, 3, 4), 2 + 12 + 24 + 10)

    def test_O0(self):
        self.check("-O0")

    def test_O2(self):
        self.check("-O2")

    def test_two_modules(self):
        p = Program(
            [
                "extern int scale; int twice(int x); int api(int x) { return twice(x) * scale; }",
                "int scale = 3; int twice(int x) { return x + x; }",
            ]
        )
        self.assertEqual(p.call("api", 7)[0], 42)

    def test_callee_saved_registers_survive(self):
        # R6-R9 belong to the caller: a function that uses them must give
        # them back.
        p = Program([CompiledCodeTest.SOURCE])
        s = p.simulator()
        p.init_variables(s)
        for reg, v in ((6, 0x6666), (7, 0x7777), (8, 0x8888), (9, 0x9999)):
            s.r[reg] = v
        s.r[0] = USER_STACK
        s.r[12], s.r[13], s.r[14], s.r[15] = 1, 2, 3, 4
        push_return(s)
        s.break_at(HALT)
        s.pc = p.address("_pressure")
        s.cont(100000)
        self.assertEqual(s.pc, HALT)
        self.assertEqual([s.r[i] for i in (6, 7, 8, 9)], [0x6666, 0x7777, 0x8888, 0x9999])
        self.assertEqual(s.r[0], USER_STACK)

    def test_line_markers_map_code_to_c(self):
        asm = compile_c("int f(int a)\n{\n  return a * 3;\n}\n", "-g")
        self.assertIn('# 3 "test.c"', asm)
        result = openc166.assemble_many([("t.a66", asm)])
        self.assertTrue(result.ok, result.errors)
        info = result.debug_info()
        sources = {
            (info["files"][l["source"]["file"]], l["source"]["line"])
            for l in info["lines"]
            if "source" in l
        }
        self.assertEqual(sources, {("test.c", 3)})


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class LongTest(unittest.TestCase):
    """32-bit arithmetic: inline ADD/ADDC and MUL, runtime division and
    shifts. A long argument takes two registers, low word first; the
    result is in R4 (low) and R5 (high)."""

    SOURCE = r"""
long add(long a, long b) { return a + b; }
long sub(long a, long b) { return a - b; }
long mul(long a, long b) { return a * b; }
long sdiv(long a, long b) { return a / b; }
long smod(long a, long b) { return a % b; }
unsigned long udiv(unsigned long a, unsigned long b) { return a / b; }
unsigned long umod(unsigned long a, unsigned long b) { return a % b; }
long shl(long a, int n) { return a << n; }
long sar(long a, int n) { return a >> n; }
unsigned long shr(unsigned long a, int n) { return a >> n; }
int lt(long a, long b) { return a < b; }
int ult(unsigned long a, unsigned long b) { return a < b; }
long widen(int a, unsigned b) { return (long)a * 1000 + b; }
long after_three(int a, int b, int c, long l) { return l + c; }
unsigned long sum(const unsigned long *p, int n) {
  unsigned long s = 0;
  while (n--) s += *p++;
  return s;
}
const unsigned long table[3] = {100000, 200000, 4000000000u};
unsigned long sum_table(void) { return sum(table, 3); }
"""

    def check(self, opt):
        p = Program([self.SOURCE], opt)

        def r(name, *args):
            words = []
            for a in args:
                if isinstance(a, tuple):  # a long: (value,)
                    v = a[0] & 0xFFFFFFFF
                    words.append([v & 0xFFFF, v >> 16])
                else:
                    words.append([a])
            lo, hi, _ = p.call_placed(name, *place(words))
            return signed32(lo | hi << 16)

        L = lambda v: (v,)
        self.assertEqual(r("add", L(70000), L(70000)), 140000)
        self.assertEqual(r("add", L(0xFFFF), L(1)), 0x10000)
        self.assertEqual(r("sub", L(0x10000), L(1)), 0xFFFF)
        self.assertEqual(r("sub", L(5), L(7)), -2)
        self.assertEqual(r("mul", L(1234), L(5678)), 1234 * 5678)
        self.assertEqual(r("mul", L(-300000), L(7)), -2100000)
        self.assertEqual(r("sdiv", L(-1000000), L(7)), -142857)
        self.assertEqual(r("smod", L(-1000000), L(7)), -1)
        self.assertEqual(r("udiv", L(4000000000), L(3)) & 0xFFFFFFFF, 1333333333)
        self.assertEqual(r("umod", L(4000000000), L(7)), 4000000000 % 7)
        self.assertEqual(r("shl", L(3), 20), 3 << 20)
        self.assertEqual(r("sar", L(-1000000), 9), -1000000 >> 9)
        self.assertEqual(r("shr", L(0x80000000), 31), 1)
        self.assertEqual(r("lt", L(-1), L(1)) & 0xFFFF, 1)
        self.assertEqual(r("lt", L(0x10000), L(0xFFFF)) & 0xFFFF, 0)
        self.assertEqual(r("ult", L(-1), L(1)) & 0xFFFF, 0)
        self.assertEqual(r("widen", 0xFFFF, 7), -993)
        # TASKING: a long that would start in R15 goes on the stack.
        self.assertEqual(r("after_three", 1, 2, 3, L(100000)), 100003)
        self.assertEqual(r("sum_table") & 0xFFFFFFFF, 4000300000)

    def test_O0(self):
        self.check("-O0")

    def test_O2(self):
        self.check("-O2")


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class LongLongTest(unittest.TestCase):
    """64-bit integers: word arithmetic inline, multiplication and division
    in the runtime. The C code checks itself (inputs are volatile, so the
    compiler cannot work the answers out) and returns a bit per failure."""

    SOURCE = r"""
typedef long long s64;
typedef unsigned long long u64;
volatile s64 a = 123456789012345LL, b = -987654321LL, c = 7;
volatile u64 ua = 0xFEDCBA9876543210ULL, ub = 0x12345ULL;
volatile int n = 37;
s64 add(s64 x, s64 y) { return x + y; }
int test(void) {
  int fail = 0;
  if (add(a, b) != 123455801358024LL) fail |= 1 << 0;
  if (a - b != 123457776666666LL) fail |= 1 << 1;
  if (a * c != 864197523086415LL) fail |= 1 << 2;
  if (b * b != 975461057789971041LL) fail |= 1 << 3;
  if (a / b != -124999LL) fail |= 1 << 4;
  if (a % b != 986541666LL) fail |= 1 << 5;
  if (ua / ub != 0xE0004FA01C4DULL) fail |= 1 << 6;
  if (ua % ub != 0x10A4FULL) fail |= 1 << 7;
  if ((ua >> n) != 0x7F6E5D4ULL) fail |= 1 << 8;
  if ((a << n) != (s64)0xC1BBEF2000000000ULL) fail |= 1 << 9;
  if ((b >> 3) != -123456791LL) fail |= 1 << 10;
  if (!(b < a) || a < b) fail |= 1 << 11;
  if (!(ub < ua)) fail |= 1 << 12;
  if ((s64)(long)b * 3 != -2962962963LL) fail |= 1 << 13;
  return fail;
}
"""

    def check(self, opt):
        p = Program([self.SOURCE], opt)
        self.assertEqual(p.call("test", max_instructions=2000000)[0], 0)

    def test_O0(self):
        self.check("-O0")

    def test_Os(self):
        self.check("-Os")


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class FloatTest(unittest.TestCase):
    """Soft float and soft double, from LLVM compiler-rt compiled for the
    C166 (runtime/lib). double is 64 bits, or 32 with -mdouble=32."""

    SOURCE = r"""
volatile float fa = 1.5f, fb = -2.25f, fq = 0.25f;
volatile double da = 1e10, db = 3.0, tiny = 1e-10;
volatile int i = -37;
volatile long l = 100000;
float fmul(float x, float y) { return x * y; }
int test(void) {
  int fail = 0;
  if (fmul(fa, fb) != -3.375f) fail |= 1 << 0;
  if (fa + fb != -0.75f) fail |= 1 << 1;
  if (fa / fq != 6.0f) fail |= 1 << 2;
  if (fa - fb != 3.75f) fail |= 1 << 3;
  if ((int)(fb * 10) != -22) fail |= 1 << 4;
  if ((long)(fa * 100000) != 150000) fail |= 1 << 5;
  if ((float)i != -37.0f) fail |= 1 << 6;
  if ((float)l / 4 != 25000.0f) fail |= 1 << 7;
  if (!(fb < fa) || fa < fb || fa == fb) fail |= 1 << 8;
  if (da * db != 3e10) fail |= 1 << 9;
  if ((long)(da / 1e5) != 100000) fail |= 1 << 10;
  if ((double)fa != 1.5) fail |= 1 << 11;
  if ((float)(db / 4) != 0.75f) fail |= 1 << 12;
#ifdef _DOUBLE_FP
  if (sizeof(double) != 8 || 1.0 + tiny == 1.0) fail |= 1 << 13;
  if ((long long)(da * db) != 30000000000LL) fail |= 1 << 14;
#else
  if (sizeof(double) != 4 || 1.0 + tiny != 1.0) fail |= 1 << 13;
#endif
  return fail;
}
"""

    def check(self, opt):
        p = Program([self.SOURCE], opt)
        self.assertEqual(p.call("test", max_instructions=5000000)[0], 0)

    def test_double_O0(self):
        self.check("-O0")

    def test_double_Os(self):
        self.check("-Os")

    def test_single_Os(self):
        self.check("-Os -mdouble=32")


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class StartupTest(unittest.TestCase):
    """runtime/cstart.a66: initialised variables get their values, the
    others are cleared, main's result comes back in R4, and the caller's
    R0 is kept."""

    def test_variables_are_set_up_before_main(self):
        p = Program(
            [
                "int a = 1234; long big = 100000; char text[] = \"C166\";\n"
                "int zero; int zeros[4];\n"
                "int other(void);\n"
                "int main(void) {\n"
                "  if (zero || zeros[3]) return -1;\n"
                "  return a + (int)(big / 1000) + text[3] + other();\n"
                "}\n",
                "static int b = 7; int other(void) { return b; }\n",
            ],
            startup=True,
        )
        # RAM starts out as garbage; __cstart must set up every variable.
        s = p.simulator()
        s.write(RAM[0], b"\xA5" * (RAM[1] - RAM[0] + 1))
        push_return(s)
        s.r[0] = 0x1234
        s.break_at(HALT)
        s.pc = p.address("__cstart")
        s.cont(200000)
        self.assertEqual(s.pc, HALT)
        self.assertEqual(s.r[4], 1234 + 100 + ord("6") + 7)
        self.assertEqual(s.r[0], 0x1234)

    def test_struct_copy_uses_memcpy(self):
        p = Program(
            [
                "struct s { int v[20]; };\n"
                "int f(struct s *a, const struct s *b) { *a = *b; return a->v[19]; }\n"
                "struct s x, y = {{1, 2, 3, [19] = 77}};\n"
                "int main(void) { return f(&x, &y); }\n"
            ],
            startup=True,
        )
        self.assertIn("_memcpy", p.result.symbols)
        self.assertEqual(p.call("__cstart")[0], 77)


def crc16(data):
    """CRC-16 with polynomial 0A001H, as the 5WY19 boot block computes it."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class TaskingExtensionsTest(unittest.TestCase):
    """The TASKING keywords and intrinsics: SFRs and their bits by name,
    _bitword and _atbit, _at(), _interrupt() with its vector and frame."""

    def test_sfrs_bits_and_fixed_addresses(self):
        p = Program(
            [
                r"""
#include <reg167.h>
#include <c166.h>
_bitword flags;
_bit ready _atbit(flags, 2);
_sfrbit P2_13 _atbit(P2, 13);
const int magic[2] _at(0x6000) = {0x1234, 0x5678};
int fixedvar _at(0xFA80);

int test(int v) {
  DP2 = 0x00FF;
  P2 = v;
  _putbit(1, P2, 12);
  P2_13 = 1;
  _bfld(P2, 0x0F00, 0x0500);
  ready = 1;
  fixedvar = magic[1];
  int r = _getbit(P2, 12) + 2 * P2_13 + 4 * ready;
  if (_testset(ready))
    r += 1000;
  if (!_testclear(ready))
    r += 2000;
  return r + 8 * _getbit(flags, 2);
}
"""
            ]
        )
        lo, _, s = p.call("test", 0x0003)
        self.assertEqual(lo, 1 + 2 + 4 + 1000)  # _testclear saw it set; now clear
        self.assertEqual(s.sfr["DP2"], 0x00FF)
        self.assertEqual(s.sfr["P2"], 0x3503)  # bits 12, 13 set; 8..11 = 5
        self.assertEqual(s.read16(0xFA80), 0x5678)
        self.assertEqual(p.address("_magic"), 0x6000)
        self.assertTrue(0xFD00 <= p.address("_flags") < 0xFE00)
        self.assertEqual(s.read16(p.address("_flags")), 0)

    def test_interrupt_function(self):
        p = Program(
            [
                r"""
#include <c166.h>
volatile int ticks;
int seen;
void _interrupt(0x22) isr(void) {
  ticks += 3;
  seen = ticks * 7 / 5;
}
int test(int a, int b) {
  int x = a * b;
  _int166(0x22);
  _int166(0x22);
  return x + a + b + ticks + seen;
}
"""
            ]
        )
        # The vector: JMPS to the function at 4 * 22H.
        vector = p.rom[0x88:0x8C]
        self.assertEqual(vector[0], 0xFA)
        self.assertEqual(vector[2] | vector[3] << 8, p.address("_isr"))
        lo, _, s = p.call("test", 3, 4)
        self.assertEqual(lo, 12 + 3 + 4 + 6 + 8)
        asm = p.asm[-1][1]
        self.assertIn("RETI", asm)
        self.assertIn("SCXT\tMDC, #0010H", asm)


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class FarHugeTest(unittest.TestCase):
    """_huge (EXTS) and _far (EXTP) pointers on the simulated 5WY19, whose
    flash appears at 0 and at 80000H: reading flash above 64 KB, and
    pointers made from near ones through the DPPs."""

    SOURCE = r"""
#include <c166.h>
unsigned crc(const unsigned char _huge *p, unsigned n) {
  unsigned c = 0xFFFF;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++)
      c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}
unsigned crc_at(unsigned seg, unsigned sof, unsigned n) {
  return crc(_mkhp(sof, seg), n);
}
long long_at(unsigned seg, unsigned sof) { return *(long _huge *)_mkhp(sof, seg); }
int x;
int roundtrip(void) {
  int _far *f = &x;
  int _huge *h = &x;
  *f = 0x1234;
  int a = *h;
  *h = 0x0042;
  /* With the reset DPPs (DPPn = n) near addresses are segment 0 ones. */
  return a + x + (_pag(&x) == _pag(f)) + (_seg(h) == 0 && _sof(h) == (unsigned)&x);
}
"""

    def test_flash_above_64k_and_conversions(self):
        image = bytes((i * 37 + (i >> 8)) & 0xFF for i in range(0x80000))
        p = Program(
            [self.SOURCE],
            image=image,
            board="5wy19",
            classes={
                "CPROGRAM": [(0xF200, 0xF8FF)],
                "CNEARROM": [(0xF200, 0xF8FF)],
                "CNEAR": [(0xF900, 0xF9FF)],
            },
        )
        lo, _, _ = p.call("crc_at", 0x8, 0x1234, 200)
        self.assertEqual(lo, crc16(image[0x1234 : 0x1234 + 200]))
        lo, hi, _ = p.call("long_at", 0x9, 0x0010)
        self.assertEqual(lo | hi << 16, int.from_bytes(image[0x10010:0x10014], "little"))
        lo, _, s = p.call("roundtrip")
        self.assertEqual(lo, 0x1234 + 0x42 + 1 + 1)
        self.assertEqual(s.read16(p.address("_x")), 0x42)


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class FarHugeVariablesTest(unittest.TestCase):
    """_huge and _far variables in the 5WY19's external RAM (chip select 2
    mapped at 90000H), their initial values and constants in flash, set up
    by __cstart."""

    SOURCE = r"""
_huge int htab[4] = {11, 22, 33, 44};
_huge long hzero[50];
const _huge char hmsg[] = "huge";
_far int fvar = 7;
_far int fzero[3];
const _far int fconst[2] = {10, 20};
int _huge *hp = &htab[2];
volatile int i = 3;
int main(void) {
  int fail = 0;
  if (htab[1] != 22) fail |= 1;
  if (hzero[49] != 0 || hzero[i] != 0) fail |= 2;
  if (hmsg[i] != 'e') fail |= 4;
  if (fvar != 7 || fzero[2] != 0) fail |= 8;
  if (fconst[i - 2] != 20) fail |= 16;
  if (*hp != 33) fail |= 32;
  htab[i] = 99;
  fzero[1] = 1234;
  hzero[i] = 100000;
  if (htab[3] != 99 || fzero[1] != 1234 || hzero[3] != 100000) fail |= 64;
  return fail;
}
"""

    def test_variables_above_64k(self):
        image = bytes(0x80000 * [0xFF])
        p = Program(
            [self.SOURCE],
            "-Os",
            startup=True,
            image=image,
            board="5wy19",
            classes={
                "CPROGRAM": [(0x10000, 0x1FFFF)],
                "CNEARROM": [(0x8000, 0xBFFF)],
                "CNEAR": [(0xF600, 0xF9FF)],
                "CHUGE": [(0x90000, 0x93FFF)],
                "CFAR": [(0x94000, 0x97FFF)],
                "CHUGEROM": [(0x20000, 0x2FFFF)],
                "CFARROM": [(0x30000, 0x33FFF)],
            },
        )
        self.assertEqual(p.address("_htab") >> 16, 9)
        self.assertEqual(p.address("_fvar") >> 16, 9)
        self.assertEqual(p.address("_hmsg") >> 16, 2)
        s = p.simulator()
        s.sfr["ADDRSEL2"] = 0x0904  # 64 KB at 90000H
        s.sfr["BUSCON2"] = 0x0480  # BUSACT2, 16-bit demultiplexed
        s.write(0x90000, b"\xA5" * 0x8000)  # RAM starts out as garbage
        push_return(s)
        s.r[0] = USER_STACK
        s.break_at(HALT)
        s.pc = p.address("__cstart")
        s.cont(1000000)
        self.assertEqual(s.pc, HALT)
        self.assertEqual(s.r[4], 0)  # a bit per failed check
        self.assertEqual(s.read16(p.address("_htab") + 6), 99)
        asm = p.asm[-1][1]
        self.assertIn("EXTS\t#SEG (_htab+2), #1", asm)
        self.assertIn("EXTS\t#SEG _fvar, #1", asm)


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class FarCodeTest(unittest.TestCase):
    """The small memory model (TASKING's): functions are far, called with
    CALLS and returning with RETS, and function pointers are segment offset
    and segment. Here code lies in segments 1 and 3 of the 5WY19's flash."""

    MAIN = r"""
typedef int (*fn)(int);
extern const fn ops[2];
int far_add(int a, int b);
static int twice(int x) { return x + x; }      /* near: CALL/RET */
int call(fn f, int x) { return f(x); }          /* through __c166_icall */
int test(int x) {
  return far_add(x, 1000) + call(ops[0], x) + ops[1](x) + twice(x);
}
int fpsize(void) { return sizeof(fn); }
"""
    OTHER = r"""
typedef int (*fn)(int);
int far_add(int a, int b) { return a + b; }
static int square(int x) { return x * x; }
static int negate(int x) { return -x; }
const fn ops[2] = { square, negate };
"""

    def test_calls_across_segments(self):
        image = bytes(0x80000 * [0xFF])
        p = Program(
            [self.MAIN, self.OTHER],
            "-Os",
            image=image,
            board="5wy19",
            classes={
                "CPROGRAM": [(0x10000, 0x1FFFF)],
                "CNEARROM": [(0x8000, 0xBFFF)],
                "CNEAR": [(0xF900, 0xF9FF)],
            },
            sections={"M1_PR": 0x30000},
        )
        self.assertEqual(p.address("_test") >> 16, 1)
        self.assertEqual(p.address("_far_add") >> 16, 3)
        lo, _, _ = p.call("test", 7)
        self.assertEqual(lo, (7 + 1000) + 49 + (-7 & 0xFFFF) + 14 & 0xFFFF)
        self.assertEqual(p.call("fpsize")[0], 4)
        asm = p.asm[1][1]  # m0
        self.assertIn("CALLS\tSEG _far_add, SOF _far_add", asm)
        self.assertIn("CALLS\tSEG ___c166_icall, SOF ___c166_icall", asm)
        self.assertIn("RETS", asm)
        # openc166 assembles all of it without a warning (OC35: this test
        # has no variables for the CNEAR range).
        self.assertEqual([w for w in p.result.warnings if w.code != "OC35"], [])

    def test_tiny_model(self):
        p = Program([CompiledCodeTest.SOURCE], "-Os", tiny=True)
        self.assertEqual(p.call("fib", 10)[0], 55)
        self.assertEqual(p.call("call_five")[0], 10010)
        self.assertNotIn("RETS", p.asm[-1][1])


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class FarDataModelsTest(unittest.TestCase):
    """The medium and large memory models (TASKING's): data is far.
    Unqualified pointers are 32-bit and variables go into the huge
    sections, here in the 5WY19's external RAM at 90000H; locals stay on
    the near user stack, and their addresses are made far through the DPPs.
    Medium's code is near, large's far."""

    SOURCE = r"""
#include <stdarg.h>
struct rec { int a; long b; char name[6]; };
struct rec recs[3] = {{1, 100000, "one"}, {2, -5, "two"}};
struct rec copy;
int zeros[40];
const char text[] = "far text";
_near int nearvar = 77;
_system int sysvar;
int *gp = &zeros[5];
static int twice(int x) { return 2 * x; }
int (*fp)(int) = twice;
volatile int three = 3;

int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int t = 0;
  while (n--)
    t += va_arg(ap, int);
  va_end(ap);
  return t;
}
long lsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long t = 0;
  while (n--)
    t += va_arg(ap, long);
  va_end(ap);
  return t;
}
int count(const char *s, char c) { int n = 0; while (*s) n += *s++ == c; return n; }
void fill(int *p, int n, int v) { while (n--) *p++ = v; }

int main(void) {
  int fail = 0;
  int local[8];
  if (recs[0].b != 100000 || recs[1].b != -5 || recs[2].a != 0) fail |= 1;
  copy = recs[1];
  if (copy.a != 2 || copy.name[1] != 'w') fail |= 2;
  if (count(text, 'a') != 1 || count(text, 't') != 2 || text[three] != ' ') fail |= 4;
  fill(local, 8, 9);                  /* a local through a far pointer */
  fill(zeros + three, 2, 5);
  if (local[7] != 9 || zeros[4] != 5 || zeros[5] != 0 || *gp != 0) fail |= 8;
  if (sum(3, 1, 2, 3) != 6 || lsum(2, 70000L, 5L) != 70005) fail |= 16;
  if (fp(21) != 42 || nearvar != 77) fail |= 32;
  sysvar += 5;
  if (sysvar != 5) fail |= 64;
  if (sizeof(int *) != 4 || sizeof(fp) != (_MODEL == 'm' ? 2 : 4)) fail |= 128;
  return fail;
}
"""

    def run_model(self, model, opt):
        image = bytes(0x80000 * [0xFF])
        code = (0x1000, 0x7FFF) if model == "medium" else (0x10000, 0x1FFFF)
        p = Program(
            [self.SOURCE],
            opt,
            startup=True,
            image=image,
            board="5wy19",
            model=model,
            classes={
                "CPROGRAM": [code],
                "CNEARROM": [(0x8000, 0xBFFF)],
                "CNEAR": [(0xF600, 0xF9FF)],
                "CHUGE": [(0x90000, 0x93FFF)],
                "CHUGEROM": [(0x20000, 0x2FFFF)],
            },
        )
        self.assertEqual(p.address("_recs") >> 16, 9)
        self.assertEqual(p.address("_text") >> 16, 2)
        self.assertEqual(p.address("_nearvar") >> 16, 0)
        s = p.simulator()
        s.sfr["ADDRSEL2"] = 0x0904  # 64 KB at 90000H
        s.sfr["BUSCON2"] = 0x0480  # BUSACT2, 16-bit demultiplexed
        s.write(0x90000, b"\xA5" * 0x8000)  # RAM starts out as garbage
        push_return(s, far=p.far_code)
        s.r[0] = USER_STACK
        s.break_at(HALT)
        s.pc = p.address("__cstart")
        stop = s.cont(2000000)
        self.assertEqual(s.pc, HALT, stop)
        self.assertEqual(s.r[4], 0)  # a bit per failed check
        self.assertEqual(s.read16(p.address("_copy")), 2)
        return p

    def test_medium(self):
        for opt in ("-O0", "-Os"):
            with self.subTest(opt=opt):
                asm = self.run_model("medium", opt).asm[-1][1]
                self.assertIn("CALL\t_sum", asm)
                self.assertNotIn("CALLS", asm)

    def test_large(self):
        for opt in ("-O0", "-Os"):
            with self.subTest(opt=opt):
                asm = self.run_model("large", opt).asm[-1][1]
                self.assertIn("CALLS\tSEG _sum, SOF _sum", asm)


# Where the C library tests put things on the 5WY19: code below 64 KB for
# the near-code models, at 10000H for the far ones; near data in the
# internal RAM; far data in the external RAM at 90000H.
def libc_program(sources, model, opt="-Os", big=False):
    code = [(0x1000, 0x7FFF)] if model in ("tiny", "medium") else [(0x10000, 0x1FFFF)]
    near_rom = [(0x8000, 0xBFFF)]
    near_ram = [(0xF200, 0xF9FF)]
    if big:  # all the math functions: code in six segments, and the C167CS's
        assert model == "large"  # XRAM2 for their user stack
        code = [(0x10000, 0x1FFFF), (0x30000, 0x7FFFF)]
        near_ram = [(0xC000, 0xD7FF)] + near_ram
    return Program(
        sources,
        opt,
        startup=True,
        image=bytes(0x80000 * [0xFF]),
        board="5wy19",
        model=model,
        classes={
            "CPROGRAM": code,
            "CNEARROM": near_rom,
            "CNEAR": near_ram,
            "CHUGE": [(0x90000, 0x93FFF)],
            "CHUGEROM": [(0x20000, 0x2FFFF)],
        },
    )


def run_cstart(p, instructions=3000000):
    """Calls __cstart as a loader would; returns the simulator at the end.
    The watchdog is frozen: the library does not service it, and the 64-bit
    divisions of printf("%lld") take longer than its period."""
    s = p.simulator()
    s.watchdog_frozen = True
    s.sfr["ADDRSEL2"] = 0x0904  # 64 KB of external RAM at 90000H
    s.sfr["BUSCON2"] = 0x0480
    s.sfr["SYSCON"] |= 0x0004  # XPEN: the on-chip XRAM
    s.sfr["XPERCON"] |= 0x0800  # XRAM2 at 0C000H
    push_return(s, far=p.far_code)
    s.r[0] = USER_STACK
    for reg in (6, 7, 8, 9):
        s.r[reg] = 0x1100 * reg
    s.break_at(HALT)
    s.pc = p.address("__cstart")
    stop = s.cont(instructions)
    # A trap runs on through the empty vectors into HALT too: R0 tells.
    if s.pc != HALT or s.r[0] != USER_STACK:
        raise AssertionError(f"__cstart did not return: {stop}, pc={s.pc:06X}, "
                             f"R0={s.r[0]:04X}, TFR={s.sfr['TFR']:04X}")
    return s


MODELS = ("tiny", "small", "medium", "large")


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class CLibraryTest(unittest.TestCase):
    """openc166's C library (runtime/libc, headers in runtime/include) in
    every memory model."""

    # Output goes to the program's own _write, so the library's (ASC0) is
    # not linked; a small heap of its own, too.
    SOURCE = r"""
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char out[64];
int outn;
size_t _write(int fd, char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (outn < (int)sizeof out)
      out[outn++] = b[i];
  return n;
}
char __c166_heap[300];
const size_t __c166_heap_size = sizeof __c166_heap;

static jmp_buf jb;
static int cmp(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}
static void deep(int n) {
  if (n == 0)
    longjmp(jb, 42);
  deep(n - 1);
}

int main(void) {
  int fail = 0;
  char buf[80];
  strcpy(buf, "hello");
  strcat(buf, ", world");
  if (strlen(buf) != 12 || strcmp(buf, "hello, world") ||
      strncmp(buf, "help", 3) || !strncmp(buf, "help", 4))
    fail |= 1;
  if (strchr(buf, 'o') != buf + 4 || strrchr(buf, 'o') != buf + 8 ||
      strstr(buf, "wor") != buf + 7 || strpbrk(buf, " ,") != buf + 5 ||
      strspn(buf, "hel") != 4 || strcspn(buf, ",") != 5)
    fail |= 2;
  char tok[] = "a,b;;c";
  char *t1 = strtok(tok, ",;"), *t2 = strtok(NULL, ",;");
  char *t3 = strtok(NULL, ",;"), *t4 = strtok(NULL, ",;");
  if (strcmp(t1, "a") || strcmp(t2, "b") || strcmp(t3, "c") || t4)
    fail |= 4;
  snprintf(buf, sizeof buf, "[%5d|%-4x|%04X|%+d|%s|%.2s|%c|%lu|%lld|%%|%o|%#x|%.3d]",
           -42, 255, 0xbeef, 7, "str", "abc", 'Z', 4000000000UL,
           -1234567890123LL, 8, 255, 5);
  if (strcmp(buf, "[  -42|ff  |BEEF|+7|str|ab|Z|4000000000|-1234567890123|%|10|0xff|005]"))
    fail |= 8;
  if (snprintf(buf, 4, "%ld", 123456L) != 6 || strcmp(buf, "123"))
    fail |= 16;
  char *end;
  if (strtol("  -0x1F!", &end, 0) != -31 || *end != '!' ||
      strtoul("777", NULL, 8) != 511 || atoi("123") != 123 ||
      strtoll("9223372036854775807", NULL, 10) != 9223372036854775807LL)
    fail |= 32;
  errno = 0;
  if (strtol("99999999999", NULL, 10) != 2147483647L || errno != ERANGE)
    fail |= 64;
  int a[] = {5, 3, 9, 1, 7, 2}, key = 7;
  qsort(a, 6, sizeof a[0], cmp);
  if (a[0] != 1 || a[3] != 5 || a[5] != 9 ||
      bsearch(&key, a, 6, sizeof a[0], cmp) != &a[4])
    fail |= 128;
  /* Through volatiles: LLVM may assume what malloc returns differs from
     a freed pointer, and that a malloc whose result is unused succeeds. */
  char *volatile p = malloc(100), *volatile q = malloc(100);
  if (!p || !q || p == q)
    fail |= 256;
  free(p);
  char *volatile r = malloc(50);
  if (r != p)
    fail |= 512;
  free(q);
  free(r);
  char *volatile big = malloc(250);   /* only fits if the freed blocks merged */
  char *volatile again = malloc(250);
  char *volatile small = realloc(big, 10);
  if (!big || small != big || again)
    fail |= 1024;
  if (!isdigit('7') || isdigit('a') || toupper('q') != 'Q' ||
      !isspace('\n') || !isxdigit('F') || ispunct('a') || !ispunct('!'))
    fail |= 2048;
  volatile int tries = 0;
  int v = setjmp(jb);
  if (v == 0) {
    tries++;
    deep(5);
  }
  if (v != 42 || tries != 1)
    fail |= 4096;
  printf("n=%d;", 12);
  puts("ok");
  fputs("err", stderr);
  putchar('!');
  if (outn != 12 || memcmp(out, "n=12;ok\nerr!", 12))
    fail |= 8192;
  div_t d = div(-7, 2);
  if (d.quot != -3 || d.rem != -1 || abs(-5) != 5 || labs(-70000L) != 70000L)
    fail |= 16384;
  return fail;
}
"""

    EXIT = r"""
#include <stdlib.h>
int count;
static void f1(void) { count += 1; }
static void f2(void) { count *= 10; }
static void inner(int n) {
  if (n)
    inner(n - 1);
  exit(40 + count);
}
int main(void) {
  atexit(f1);
  atexit(f2);
  count = 2;
  inner(3);
  return 1;
}
"""

    ASSERT = r"""
#include <assert.h>
#include <stddef.h>
char out[80];
int outn;
size_t _write(int fd, char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (outn < (int)sizeof out)
      out[outn++] = b[i];
  return n;
}
volatile int two = 2;
int main(void) {
  assert(two == 2);
  assert(two + two == 5);
  return 0;
}
"""

    ASC0 = r"""
#include <reg167.h>
#include <stdio.h>
int main(void) {
  P3 |= 0x0400;
  DP3 |= 0x0400;
  S0BG = 77;
  S0CON = 0x8011;
  printf("%s %d\n", "hi", -3);
  return 0;
}
"""

    def test_library(self):
        for model in MODELS:
            with self.subTest(model=model):
                s = run_cstart(libc_program([self.SOURCE], model))
                self.assertEqual(s.r[4], 0)  # a bit per failed check

    def test_exit_and_atexit(self):
        for model in MODELS:
            with self.subTest(model=model):
                p = libc_program([self.EXIT], model)
                s = run_cstart(p)
                # exit's status, after f2 then f1; the caller's registers
                self.assertEqual(s.r[4], 42)
                self.assertEqual(s.read16(p.address("_count")), 21)
                self.assertEqual(s.r[0], USER_STACK)
                self.assertEqual([s.r[i] for i in (6, 7, 8, 9)],
                                 [0x1100 * i for i in (6, 7, 8, 9)])

    def test_assert(self):
        p = libc_program([self.ASSERT], "small")
        s = run_cstart(p)
        self.assertEqual(s.r[4], 1)  # abort: EXIT_FAILURE
        n = s.read16(p.address("_outn"))
        text = s.read(p.address("_out"), n).decode()
        self.assertEqual(text, "Assertion failed: two + two == 5, file m0.c, line 15\n")

    def test_printf_over_asc0(self):
        """The library's own _write: ASC0."""
        p = libc_program([self.ASC0], "small")
        s = p.simulator()
        s.watchdog_frozen = True
        s.serial_echo = False
        push_return(s)
        s.r[0] = USER_STACK
        s.break_at(HALT)
        s.pc = p.address("__cstart")
        s.run_for(0.02)
        self.assertEqual(s.pc, HALT)
        self.assertEqual(s.serial_receive(), b"hi -3\n")


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class StructByValueTest(unittest.TestCase):
    """Structs passed by value: a copy on the user stack, and every argument
    after it there too (TASKING); in every memory model."""

    SOURCE = r"""
struct p { int x, y; long z; char c; };
struct p g = {1, 2, 300000, 4};
__attribute__((noinline)) long take(int a, struct p s, int b) {
  s.x += 100;  /* the callee's own copy */
  return a + s.x + s.y + s.z + s.c + b;
}
__attribute__((noinline)) int two(struct p a, struct p b) { return a.c * 10 + b.c; }
int main(void) {
  struct p l = {5, 6, 7, 8};
  int fail = 0;
  if (take(10, g, 20) != 10 + 101 + 2 + 300000 + 4 + 20) fail |= 1;
  if (take(1, l, 2) != 1 + 105 + 6 + 7 + 8 + 2) fail |= 2;
  if (g.x != 1 || l.x != 5) fail |= 4;
  if (two(g, l) != 48) fail |= 8;
  return fail;
}
"""

    def test_models(self):
        for model in MODELS:
            with self.subTest(model=model):
                s = run_cstart(libc_program([self.SOURCE], model))
                self.assertEqual(s.r[4], 0)  # a bit per failed check


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class MathLibraryTest(unittest.TestCase):
    """<math.h>: LLVM libc's functions, checked against Python's."""

    # Volatile inputs, or Clang computes the results itself.
    # The functions need about 2 KB of user stack.
    SOURCE = r"""
#include <c166.h>
#include <math.h>
_ustack(3072);
double out[33];
float outf[8];
int main(void) {
  volatile double a = 0.5, b = 2.0, c = -2.5, d = 10.0, e = 7.5, f = 1e-3, g = 3.0;
  out[0] = sin(a); out[1] = cos(a); out[2] = tan(a); out[3] = asin(a);
  out[4] = acos(a); out[5] = atan(a); out[6] = atan2(a, b); out[7] = exp(a);
  out[8] = log(d); out[9] = log10(d); out[10] = pow(b, a); out[11] = sqrt(b);
  out[12] = floor(c); out[13] = ceil(c); out[14] = fmod(e, b);
  out[15] = sinh(a); out[16] = cosh(a); out[17] = tanh(a); out[18] = cbrt(g);
  out[19] = hypot(g, 2 * b); out[20] = expm1(f); out[21] = log1p(f);
  out[22] = log2(d); out[23] = exp2(a); out[24] = round(c); out[25] = trunc(c);
  out[26] = fabs(c);
  int ex;
  out[27] = frexp(d, &ex);
  out[28] = ex;
  out[29] = ldexp(a, 4);
  double ip;
  out[30] = modf(c, &ip);
  out[31] = ip;
  out[32] = lround(c);
  outf[0] = sinf(a); outf[1] = expf(a); outf[2] = logf(d); outf[3] = sqrtf(b);
  outf[4] = powf(b, a); outf[5] = sinhf(a); outf[6] = atan2f(a, b);
  outf[7] = hypotf(g, 2 * b);
  return 0;
}
"""
    A, B, C, D, E, F, G = 0.5, 2.0, -2.5, 10.0, 7.5, 1e-3, 3.0
    EXPECTED = [
        math.sin(A), math.cos(A), math.tan(A), math.asin(A), math.acos(A),
        math.atan(A), math.atan2(A, B), math.exp(A), math.log(D), math.log10(D),
        math.pow(B, A), math.sqrt(B), math.floor(C), math.ceil(C), math.fmod(E, B),
        math.sinh(A), math.cosh(A), math.tanh(A), G ** (1 / 3), math.hypot(G, 2 * B),
        math.expm1(F), math.log1p(F), math.log2(D), 2 ** A, -3.0, -2.0, 2.5,
        0.625, 4, A * 16, -0.5, -2.0, -3,
    ]
    EXPECTED_F = [math.sin(A), math.exp(A), math.log(D), math.sqrt(B),
                  math.pow(B, A), math.sinh(A), math.atan2(A, B), math.hypot(G, 2 * B)]

    def test_functions(self):
        # All of them at once take 200 KB of code: the large model, whose
        # constants are far too.
        for model in ("large",):
            with self.subTest(model=model):
                p = libc_program([self.SOURCE], model, big=True)
                s = run_cstart(p, 20000000)
                got = struct.unpack("<33d", s.read(p.address("_out"), 33 * 8))
                for i, (g, want) in enumerate(zip(got, self.EXPECTED)):
                    self.assertTrue(math.isclose(g, want, rel_tol=1e-15), (i, g, want))
                got = struct.unpack("<8f", s.read(p.address("_outf"), 8 * 4))
                for i, (g, want) in enumerate(zip(got, self.EXPECTED_F)):
                    self.assertTrue(math.isclose(g, want, rel_tol=1e-6), (i, g, want))

    def test_single_precision_double(self):
        """-mdouble=32: double is float, and sin is sinf."""
        source = r"""
#include <c166.h>
#include <math.h>
_ustack(1024);  /* sinf needs more than the 512 bytes of user stack */
double r;
int main(void) {
  volatile double a = 0.5;
  r = sin(a) + sqrt(a);
  return sizeof(double);
}
"""
        p = libc_program([source], "small", "-Os -mdouble=32")
        s = run_cstart(p, 20000000)
        self.assertEqual(s.r[4], 4)
        (r,) = struct.unpack("<f", s.read(p.address("_r"), 4))
        self.assertTrue(math.isclose(r, math.sin(0.5) + math.sqrt(0.5), rel_tol=1e-6))
        self.assertIn("_sinf", p.asm[-1][1])


@unittest.skipUnless(CLANG, "clang for C166 not found (set OPENC166_CLANG)")
class Payload5WY19Test(unittest.TestCase):
    """A MiniMon-style payload in C, loaded into the internal RAM of the
    simulated 5WY19: it sets up ASC0 (the K-line), answers commands from the
    host and returns to its caller. 'C' addr len: CRC-16 of the flash
    range, as the boot block computes it. 'Q': return."""

    PAYLOAD = r"""
#include <reg167.h>
#include <c166.h>

static unsigned char get(void) {
  while (!S0RIR)
    _srvwdt();
  S0RIR = 0;
  return S0RBUF;
}

static void put(unsigned char c) {
  S0TIR = 0;
  S0TBUF = c;
  while (!S0TIR)
    _srvwdt();
}

static unsigned get16(void) {
  unsigned lo = get();
  return lo | (unsigned)get() << 8;
}

static unsigned crc16(const unsigned char *p, unsigned n) {
  unsigned crc = 0xFFFF;
  while (n--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    _srvwdt();
  }
  return crc;
}

int main(void) {
  P3 |= 0x0400;          /* TXD0 high */
  DP3 |= 0x0400;         /* TXD0 output */
  DP3 &= ~0x0800;        /* RXD0 input */
  S0BG = 77;             /* 9600 baud at 24 MHz */
  S0CON = 0x8011;        /* 8 data bits, receiver on, running */
  for (;;) {
    unsigned char cmd = get();
    if (cmd == 'Q')
      return 0x1234;
    if (cmd == 'C') {
      const unsigned char *start = (const unsigned char *)get16();
      unsigned crc = crc16(start, get16());
      put(crc);
      put(crc >> 8);
    }
  }
}
"""

    def test_crc_over_the_serial_line(self):
        image = bytes((i * 37 + (i >> 8)) & 0xFF for i in range(0x80000))
        p = Program(
            [self.PAYLOAD],
            startup=True,
            image=image,
            board="5wy19",
            classes={
                "CPROGRAM": [(0xF200, 0xF8FF)],
                "CNEARROM": [(0xF200, 0xF8FF)],
                "CNEAR": [(0xF900, 0xFBBF)],
            },
        )
        s = p.simulator()
        s.serial_echo = False
        push_return(s)
        s.break_at(HALT)
        s.pc = p.address("__cstart")
        s.run_for(0.002)
        for start, length in ((0x1000, 64), (0x2345, 300)):
            s.serial_send(b"C" + start.to_bytes(2, "little") + length.to_bytes(2, "little"))
            s.run_for(0.02)
            got = s.serial_receive()
            self.assertEqual(got, crc16(image[start : start + length]).to_bytes(2, "little"))
        s.serial_send(b"Q")
        stop = s.run_for(0.01)
        self.assertEqual(s.pc, HALT, stop)
        self.assertEqual(s.r[4], 0x1234)


if __name__ == "__main__":
    unittest.main()
