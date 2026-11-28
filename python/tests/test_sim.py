# SPDX-License-Identifier: MIT
"""Debug information, the disassembler and the simulator from Python."""

import glob
import os
import tempfile
import time
import unittest

import openc166
from openc166 import sim

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXAMPLES = os.path.join(ROOT, "examples")

PROGRAM = """$MOD167
$SEGMENTED
$INCLUDE(REG167.INC)
TWICE   MACRO   R
        ADD     R, #1
        ADD     R, #1
        ENDM
CODE0   SECTION CODE AT 0
START:  DISWDT
        EINIT
        MOV     R4, #0
LOOP:   TWICE   R4
        MOV     0FC40H, R4
        CALLR   SUB1
AFTER:  CMP     R4, #20
        JMPR    cc_NE, LOOP
DONE:   JMPR    cc_UC, DONE
SUB1:   MOV     R5, 0A000H
        RET
CODE0   ENDS
        END
"""


class DebugInfoTest(unittest.TestCase):
    def test_every_byte_of_the_examples_has_a_line(self):
        sources = sorted(glob.glob(os.path.join(EXAMPLES, "**", "*.a66"), recursive=True))
        self.assertTrue(sources)
        for path in sources:
            with self.subTest(path=os.path.relpath(path, ROOT)):
                r = openc166.assemble_file(path, include_dirs=[os.path.dirname(path)])
                if not r.ok:
                    continue
                info = r.debug_info()
                covered = {}
                for l in info["lines"]:
                    self.assertGreater(l["line"], 0)
                    for a in range(l["address"], l["address"] + l["size"]):
                        covered[a] = l
                for c in r.chunks:
                    for a in range(c.address, c.end):
                        self.assertIn(a, covered, "byte at {:X} has no line".format(a))

    def test_macro_lines_and_symbols(self):
        r = openc166.assemble(PROGRAM, file_name="prog.a66")
        self.assertTrue(r.ok, r.errors)
        info = r.debug_info()
        self.assertEqual(info["format"], "openc166-debug")
        loop = next(s for s in info["symbols"] if s["name"] == "LOOP")
        first = next(l for l in info["lines"] if l["address"] == loop["value"])
        self.assertEqual(first["line"], 12)  # the call site
        self.assertEqual(first["macro"]["line"], 5)  # the body line

    def test_include_chain(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "inner.inc"), "w") as f:
                f.write("        NOP\n")
            src = "PROG SECTION CODE AT 100H\n        NOP\n$INCLUDE(inner.inc)\nPROG ENDS\n END\n"
            r = openc166.assemble(src, file_name=os.path.join(d, "main.a66"))
            self.assertTrue(r.ok, r.errors)
            info = r.debug_info()
            inc = [l for l in info["lines"] if "include" in l]
            self.assertEqual(len(inc), 1)
            self.assertEqual(info["files"][inc[0]["file"]].lower()[-9:], "inner.inc")
            self.assertEqual(inc[0]["include"][0][1], 3)


    def test_line_markers(self):
        src = (
            "$MOD167\nPROG SECTION CODE AT 100H\n"
            "        NOP\n"
            "# 42 \"main.c\"\n"
            "        NOP\n"
            "        NOP\n"
            "PROG ENDS\n END\n"
        )
        r = openc166.assemble(src, file_name="prog.a66")
        self.assertTrue(r.ok, r.errors)
        info = r.debug_info()
        marked = [l for l in info["lines"] if "source" in l]
        self.assertEqual([(l["line"], info["files"][l["source"]["file"]], l["source"]["line"]) for l in marked],
                         [(5, "main.c", 42), (6, "main.c", 42)])
        syms = sim.Symbols()
        syms.load_debug_info(info)
        first, second = syms.line_at(0x100), syms.line_at(0x102)
        self.assertEqual((first.file, first.line, first.asm), ("prog.a66", 3, ("prog.a66", 3)))
        self.assertEqual((second.file, second.line, second.asm), ("main.c", 42, ("prog.a66", 5)))
        # Breakpoints by C line or by assembly line.
        self.assertEqual(syms.address_of_line("main.c", 42), 0x102)
        self.assertEqual(syms.address_of_line("prog.a66", 6), 0x104)


class DisassemblerTest(unittest.TestCase):
    def test_names_and_targets(self):
        out = sim.disassemble(bytes.fromhex("e60806f2" "0dff" "da084680" "1a89e3ff"), 0x88046)
        self.assertEqual([i.text for i in out], [
            "MOV     CP, #0F206H",
            "JMPR    8804AH",
            "CALLS   8, 8046H",
            "BFLDH   SYSCON, #0FFH, #0E3H",
        ])
        self.assertEqual(out[1].target, 0x8804A)
        self.assertEqual(out[2].target, 0x88046)

    def test_examples_round_trip(self):
        for path in sorted(glob.glob(os.path.join(EXAMPLES, "**", "*.a66"), recursive=True)):
            with self.subTest(path=os.path.relpath(path, ROOT)):
                r = openc166.assemble_file(path, include_dirs=[os.path.dirname(path)])
                if not r.ok:
                    continue
                img = r.image()
                text, fallbacks = sim.disassemble_module(img.data, img.base)
                self.assertEqual(fallbacks, 0)
                again = openc166.assemble(text)
                self.assertTrue(again.ok, again.errors[:3])
                self.assertEqual(again.image(base=img.base).data[: len(img.data)], img.data)


C_PROGRAM = """$MOD167
CODE0   SECTION CODE AT 0
# 1 "main.c"
START:  MOV     R4, #0
# 2
LOOP:   ADD     R4, #1
        CALLR   SUBR
        CMP     R4, #3
# 3
        JMPR    cc_NE, LOOP
# 4
DONE:   JMPR    cc_UC, DONE
# 10
SUBR:   MOV     R5, #1
        MOV     R6, #2
# 11
        RET
# 0
NOLINE: NOP
CODE0   ENDS
        END
"""


class LineStepTest(unittest.TestCase):
    def setUp(self):
        r = openc166.assemble(C_PROGRAM, file_name="prog.a66")
        self.assertTrue(r.ok, r.errors)
        self.s = sim.Simulator.from_result(r)
        self.addr = {n: r.symbol(n).value for n in ("START", "LOOP", "SUBR", "DONE", "NOLINE")}

    def tearDown(self):
        self.s.close()

    def where(self):
        line = self.s.symbols.line_at(self.s.pc)
        return (line.file, line.line)

    def test_next_steps_over_calls_and_lines(self):
        s = self.s
        self.assertEqual(self.where(), ("main.c", 1))
        self.assertEqual(s.step_line(over=True).reason, "none")
        self.assertEqual((self.where(), s.pc), (("main.c", 2), self.addr["LOOP"]))
        s.step_line(over=True)  # ADD, CALLR (over SUBR), CMP
        self.assertEqual(self.where(), ("main.c", 3))
        self.assertEqual(s.r[5], 1)
        s.step_line(over=True)  # the jump back
        self.assertEqual(s.pc, self.addr["LOOP"])

    def test_step_in_and_return_to_the_next_line(self):
        s = self.s
        s.step_line()
        s.step_line()  # ADD, then into SUBR
        self.assertEqual((self.where(), s.pc), (("main.c", 10), self.addr["SUBR"]))
        s.step_line()
        self.assertEqual(self.where(), ("main.c", 11))
        # RET lands on CMP, in the middle of line 2: the rest of it runs.
        s.step_line()
        self.assertEqual(self.where(), ("main.c", 3))

    def test_a_breakpoint_inside_the_line_stops_it(self):
        s = self.s
        s.step_line(over=True)
        cmp = self.addr["LOOP"] + 4  # ADD R4, #1 (2), CALLR (2)
        s.break_at(cmp)
        stop = s.step_line(over=True)
        self.assertEqual((stop.reason, s.pc), ("breakpoint", cmp))
        # So does one in the routine that a step over runs.
        s.break_at("SUBR")
        s.r[4] = 0
        s.pc = self.addr["LOOP"]
        self.assertEqual(s.step_line(over=True).reason, "breakpoint")
        self.assertEqual(s.pc, self.addr["SUBR"])

    def test_lines_of_assembly_and_no_lines(self):
        s = self.s
        s.pc = self.addr["NOLINE"]
        self.assertEqual(self.where(), ("prog.a66", 19))  # after # 0: the assembly line
        s.step_line()
        self.assertEqual(s.pc, self.addr["NOLINE"] + 2)
        # Past the last line: one instruction.
        s.step_line()
        self.assertEqual(s.pc, self.addr["NOLINE"] + 4)


class SimulatorTest(unittest.TestCase):
    def setUp(self):
        r = openc166.assemble(PROGRAM, file_name="prog.a66")
        self.assertTrue(r.ok, r.errors)
        self.s = sim.Simulator.from_result(r)

    def tearDown(self):
        self.s.close()

    def test_breakpoint_by_symbol_and_line(self):
        s = self.s
        s.break_at("AFTER", condition=lambda m: m.r[4] == 10)
        self.assertEqual(s.cont(10000).reason, "breakpoint")
        self.assertEqual(s.r[4], 10)
        self.assertEqual(s.pc, s.symbols.by_name["AFTER"])
        s.break_at("prog.a66:13")
        s.cont(10000)
        self.assertEqual(s.symbols.line_at(s.pc).line, 13)
        self.assertIn("prog.a66:13", s.where())

    def test_watch_and_reverse(self):
        s = self.s
        s.break_at("AFTER", condition=lambda m: m.r[4] == 16)
        s.cont(10000)
        n = s.instructions
        s.step_over()
        s.step()
        s.reverse_step()
        s.reverse_step()
        self.assertEqual(s.instructions, n)
        s.cont(5)
        s.delete(1)  # the breakpoint at AFTER would be hit first
        s.watch(0xFC40, "write")
        stop = s.reverse_continue()
        self.assertEqual(stop.reason, "watchpoint")
        self.assertEqual(stop.access[0], "write")
        self.assertEqual(stop.access[3], s.read16(0xFC40))

    def test_bit_watch(self):
        s = self.s
        s.watch("PSW.Z", "change")
        stop = s.cont(10000)
        self.assertEqual(stop.reason, "watchpoint")

    def test_snapshots_and_memory(self):
        s = self.s
        s.cont(50)
        snap = s.save()
        r4 = s.r[4]
        s.cont(50)
        s.load(snap)
        self.assertEqual(s.r[4], r4)
        self.assertEqual(s.instructions, snap.instructions)
        s.write(0xFC60, b"\x12\x34")
        self.assertEqual(s.read16(0xFC60), 0x3412)
        self.assertEqual(s.describe(0xFC60), "IRAM")
        self.assertEqual(s.sfr["CP"], 0xFC00)

    def test_block_writes(self):
        s = self.s
        s.cont(10)
        snap = s.save()
        rom = s.read(0x0000, 0x40)
        # blocks into the ROM image and RAM are fast: the decode cache is
        # cleared once per block, the image copied once per snapshot
        t = time.perf_counter()
        for _ in range(64):
            s.write(0x0000, bytes(range(0x40)))
        s.write(0xF600, bytes(0x800))
        self.assertLess(time.perf_counter() - t, 0.5)
        self.assertEqual(s.read(0x0010, 4), b"\x10\x11\x12\x13")
        s.load(snap)
        self.assertEqual(s.read(0x0000, 0x40), rom)
        # code written in a block runs as written, not from the decode cache
        s.pc = 0
        s.step()
        s.pc = 0
        s.write(0x0000, bytes.fromhex("e6f43412"))  # MOV R4, #1234H
        s.step()
        self.assertEqual(s.r[4], 0x1234)

    def test_python_device(self):
        s = self.s
        writes = []
        s.attach(1, read=lambda a: 0x5A if a == 0xA000 else 0xA5, write=lambda a, v: writes.append((a, v)), name="probe")
        s.sfr["ADDRSEL1"] = 0x00A0  # 4 KB at 0A000H
        s.sfr["BUSCON1"] = 0x0400  # BUSACT1
        self.assertEqual(s.describe(0xA000), "CS1:probe")
        s.break_at("SUB1")
        s.cont(1000)
        s.step()
        self.assertEqual(s.r[5], 0xA55A)

    def test_unmodelled(self):
        s = self.s
        s.cont(100)
        # 0A000H is in the CS0 window, which holds the ROM: reads are fine.
        self.assertEqual(s.unmodelled, [])
        s.write16(0xFC70, 0)
        s.pc = 0  # rerun from the start with a peripheral access patched in
        s.write(0x0000, bytes.fromhex("f2f440fe"))  # MOV R4, T2 (GPT1: not modelled)
        s.step()
        self.assertEqual([u.what for u in s.unmodelled], ["SFR T2 (00FE40H)"])

    def test_disassemble_at_pc(self):
        s = self.s
        out = s.disassemble(0, 3)
        self.assertEqual(out[0].text, "DISWDT")
        self.assertEqual(out[1].text, "EINIT")



if __name__ == "__main__":
    unittest.main()
