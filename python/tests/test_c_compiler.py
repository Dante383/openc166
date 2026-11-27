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
RAM = (0xF600, 0xF7FF)
USER_STACK = 0xFA00
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

    def __init__(self, sources, opt="-O2"):
        modules = [("harness.a66", HARNESS)]
        for i, src in enumerate(sources):
            modules.append((f"m{i}.a66", compile_c(src, opt, name=f"m{i}.c")))
        self.asm = modules
        self.result = openc166.assemble_many(
            modules,
            classes={
                "CPROGRAM": [ROM],
                "CNEARROM": [ROM],
                "CNEAR": [RAM],
                "CINITIRAM": [RAM],
            },
        )
        if not self.result.ok:
            listing = "\n".join(str(e) for e in self.result.errors)
            raise AssertionError("openc166 failed:\n" + listing + "\n" + modules[-1][1])
        rom = bytearray(b"\xff" * 0x8000)
        self.ram_chunks = []
        for chunk in self.result.chunks:
            if chunk.address < 0x8000:
                rom[chunk.address : chunk.end] = chunk.data
            else:
                self.ram_chunks.append(chunk)
        self.rom = bytes(rom)

    def address(self, name):
        return self.result.symbol(name).value

    def call(self, name, *args, max_instructions=200000):
        """Calls a C function with 16-bit arguments; returns R4, R5 and the
        simulator."""
        s = sim.Simulator(self.rom, board="c167")
        for chunk in self.ram_chunks:
            s.write(chunk.address, chunk.data)
        stack = []
        regs = [12, 13, 14, 15]
        for a in args:
            if regs:
                s.r[regs.pop(0)] = a & 0xFFFF
            else:
                stack.append(a & 0xFFFF)
        r0 = USER_STACK - 2 * len(stack)
        for i, v in enumerate(stack):
            s.write16(r0 + 2 * i, v)
        s.r[0] = r0
        sp = s.sfr["SP"] - 2
        s.sfr["SP"] = sp
        s.write16(sp, HALT)
        s.break_at(HALT)
        s.pc = self.address("_" + name)
        stop = s.cont(max_instructions)
        if s.pc != HALT:
            raise AssertionError(f"{name} did not return: {stop}, pc={s.pc:06X}")
        if s.r[0] != r0:
            raise AssertionError(f"{name} left R0 at {s.r[0]:04X}, not {r0:04X}")
        return s.r[4], s.r[5], s


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


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
        s = sim.Simulator(p.rom, board="c167")
        for chunk in p.ram_chunks:
            s.write(chunk.address, chunk.data)
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


if __name__ == "__main__":
    unittest.main()
