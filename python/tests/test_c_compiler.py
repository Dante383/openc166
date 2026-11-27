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

import os
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
            [CLANG, "--target=c166", opt, "-S", "-o", "-", name],
            capture_output=True,
            text=True,
            cwd=tmp,
        )
        if out.returncode != 0:
            raise AssertionError("clang failed:\n" + out.stderr)
        return out.stdout


class Program:
    """C modules linked with openc166 and loaded into a simulated C167."""

    def __init__(self, sources, opt="-O2", startup=False, classes=None, image=None, board="c167"):
        modules = [("harness.a66", HARNESS)]
        if startup:
            with open(os.path.join(RUNTIME, "cstart.a66")) as f:
                modules.append(("cstart.a66", f.read()))
        for i, src in enumerate(sources):
            modules.append((f"m{i}.a66", compile_c(src, opt, name=f"m{i}.c")))
        self.asm = modules
        self.board = board
        self.result = openc166.assemble_many(
            modules,
            classes=classes or {"CPROGRAM": [ROM], "CNEARROM": [ROM], "CNEAR": [RAM]},
            libraries=[os.path.join(RUNTIME, "lib")],
        )
        if not self.result.ok:
            listing = "\n".join(str(e) for e in self.result.errors)
            raise AssertionError("openc166 failed:\n" + listing + "\n" + modules[-1][1])
        # Without an image, the program is the ROM; with one (a flash image),
        # the program is loaded into RAM as a loader would.
        rom = bytearray(image if image is not None else b"\xff" * 0x8000)
        self.loaded = []
        for chunk in self.result.chunks:
            if image is None:
                assert chunk.end <= len(rom), "only the ROM has bytes in the image"
                rom[chunk.address : chunk.end] = chunk.data
            else:
                self.loaded.append(chunk)
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
        sp = s.sfr["SP"] - 2
        s.sfr["SP"] = sp
        s.write16(sp, HALT)
        s.break_at(HALT)
        s.pc = self.address(name if name.startswith("_") else "_" + name)
        stop = s.cont(max_instructions)
        if s.pc != HALT:
            raise AssertionError(f"{name} did not return: {stop}, pc={s.pc:06X}")
        if s.r[0] != r0:
            raise AssertionError(f"{name} left R0 at {s.r[0]:04X}, not {r0:04X}")
        return s.r[4], s.r[5], s


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
        sp = s.sfr["SP"] - 2
        s.sfr["SP"] = sp
        s.write16(sp, HALT)
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
        sp = s.sfr["SP"] - 2
        s.sfr["SP"] = sp
        s.write16(sp, HALT)
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
        sp = s.sfr["SP"] - 2
        s.sfr["SP"] = sp
        s.write16(sp, HALT)
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
