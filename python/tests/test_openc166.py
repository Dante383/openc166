# SPDX-License-Identifier: MIT
"""Tests of the openc166 Python package.

Run from the repository root after "pip install .":
    python -m unittest discover -s python/tests
or against a build tree (ctest does this):
    OPENC166_LIBRARY=build/src/libopenc166.so PYTHONPATH=python python -m unittest discover -s python/tests
"""

import doctest
import os
import re
import shutil
import subprocess
import tempfile
import threading
import unittest

import openc166

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXAMPLES = os.path.join(ROOT, "examples")


class VersionTest(unittest.TestCase):
    def test_version_matches_cmake_project(self):
        with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
            m = re.search(r"project\(openc166\s+VERSION\s+([0-9.]+)", f.read())
        self.assertEqual(openc166.__version__, m.group(1))


class DocTest(unittest.TestCase):
    def test_docstring_examples(self):
        self.assertEqual(doctest.testmod(openc166).failed, 0)


class ExampleTest(unittest.TestCase):
    """The MTOS payloads must give the bytes of gkflasher's mtos_asm.py."""

    def test_marker(self):
        result = openc166.assemble_file(os.path.join(EXAMPLES, "mtos", "marker.a66"))
        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.cpu, "c167")
        image = result.image()
        self.assertEqual(image.base, 0xC014)
        self.assertEqual(image.data.hex(), "e6f4efbef6f402c0db00")
        self.assertEqual(bytes(image), image.data)
        self.assertEqual(len(image), 10)
        self.assertEqual(image.end, 0xC01E)

    def test_memory_dump(self):
        result = openc166.assemble_file(os.path.join(EXAMPLES, "mtos", "memory_dump.a66"))
        self.assertEqual(
            result.image().data.hex(),
            "6eb6e6f40001e6f54000"
            "a758a7a7d7400200a904f7f0b0fe9ab6fe707eb6084128513df3"
            "e75800009ab6fe707eb66fb6db00",
        )

    def test_driver_patch_has_two_chunks(self):
        result = openc166.assemble_file(os.path.join(EXAMPLES, "mtos", "driver_patch.a66"))
        self.assertEqual(len(result.chunks), 2)
        self.assertEqual(result.chunks[1].data.hex(), "bb94cc00")
        gap = result.chunks[1].address - result.chunks[0].end
        self.assertEqual(gap, 204)
        image = result.image(fill=0)
        self.assertEqual(image.data[result.chunks[0].end - image.base], 0)


class CommandTest(unittest.TestCase):
    """The openc166 command that "pip install" puts on PATH.

    Skipped when it is not installed, unless OPENC166_TEST_COMMAND is set.
    """

    def setUp(self):
        self.program = shutil.which("openc166")
        if self.program is None:
            if os.environ.get("OPENC166_TEST_COMMAND"):
                self.fail("the openc166 command is not on PATH")
            self.skipTest("the openc166 command is not installed")

    def run_program(self, *args):
        return subprocess.run([self.program, *args], capture_output=True, text=True)

    def test_assembles_example(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "marker.bin")
            proc = self.run_program(os.path.join(EXAMPLES, "mtos", "marker.a66"), "-o", out)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            with open(out, "rb") as f:
                self.assertEqual(f.read().hex(), "e6f4efbef6f402c0db00")

    def test_hex_and_srec_output(self):
        source = os.path.join(EXAMPLES, "mtos", "marker.a66")
        with tempfile.TemporaryDirectory() as tmp:
            for name, args, start in (
                ("m.hex", (), ":020000040000FA\r\n:0AC01400E6F4EFBEF6F402C0DB00"),
                ("m.s37", (), "S0030000FC\r\nS30F0000C014E6F4EFBE"),
                ("m.txt", ("--format", "srec"), "S0030000FC\r\nS10DC014E6F4EFBE"),
            ):
                out = os.path.join(tmp, name)
                proc = self.run_program(source, "-o", out, *args)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                with open(out, "rb") as f:
                    self.assertTrue(f.read().decode("ascii").startswith(start), name)

    def test_error_exit_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "bad.a66")
            with open(src, "w") as f:
                f.write(" MOV R1, NOWHERE\n END\n")
            proc = self.run_program(src, "-o", os.path.join(tmp, "bad.bin"))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("bad.a66:1:10: error A42", proc.stdout + proc.stderr)
            self.assertFalse(os.path.exists(os.path.join(tmp, "bad.bin")))


class AssembleTest(unittest.TestCase):
    def test_str_and_bytes(self):
        src = " MOV R4, #0BEEFH\n RET\n END\n"
        self.assertEqual(openc166.assemble(src).image().data.hex(), "e6f4efbecb00")
        self.assertEqual(openc166.assemble(src.encode()).image().data.hex(), "e6f4efbecb00")
        self.assertEqual(openc166.assemble(bytearray(src.encode())).image().data.hex(), "e6f4efbecb00")
        with self.assertRaises(TypeError):
            openc166.assemble(42)

    def test_non_ascii(self):
        utf8 = openc166.assemble(" DB 'é' ; é\n END\n")
        self.assertEqual(utf8.image().data, "é".encode("utf-8"))
        cp1252 = openc166.assemble(" DB 'é' ; é\n END\n".encode("cp1252"))
        self.assertEqual(cp1252.image().data, b"\xe9")
        self.assertIn("\ufffd", cp1252.listing)  # not UTF-8: shown as a replacement character

    def test_origin(self):
        result = openc166.assemble("L: JMPR cc_UC, L\n END\n", origin=0xE000)
        self.assertEqual(result.image().base, 0xE000)
        self.assertEqual(result.symbols["L"].value, 0xE000)
        with self.assertRaises(ValueError):
            openc166.assemble(" NOP\n", origin=-1)

    def test_cpu(self):
        src = " EXTR #1\n MOV 0F100H, R1\n END\n"  # EXTR is C167 only
        self.assertFalse(openc166.assemble(src).ok)
        self.assertFalse(openc166.assemble(src, cpu="c166").ok)
        result = openc166.assemble(src, cpu="C167")
        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.cpu, "c167")
        with self.assertRaises(ValueError):
            openc166.assemble(src, cpu="z80")

    def test_empty_source(self):
        result = openc166.assemble("")
        self.assertEqual(result.chunks, [])
        self.assertEqual(result.image().data, b"")


class DiagnosticTest(unittest.TestCase):
    def setUp(self):
        self.result = openc166.assemble(" NOP\n MOV R1, NOWHERE\n", file_name="t.a66")

    def test_diagnostics(self):
        r = self.result
        self.assertFalse(r.ok)
        self.assertEqual(len(r.errors), 1)
        self.assertEqual(len(r.warnings), 1)  # no END statement
        e = r.errors[0]
        self.assertTrue(e.is_error)
        self.assertEqual((e.severity, e.code, e.file, e.line, e.column), ("error", "A42", "t.a66", 2, 10))
        self.assertEqual(e.message, "undefined identifier 'NOWHERE'")
        self.assertEqual(str(e), "t.a66:2:10: error A42: undefined identifier 'NOWHERE'")
        self.assertEqual(r.warnings[0].code, "A89")
        self.assertIn("errors=1", repr(r))

    def test_image_raises(self):
        with self.assertRaises(openc166.AssemblyError) as ctx:
            self.result.image()
        self.assertIs(ctx.exception.result, self.result)
        self.assertEqual([d.code for d in ctx.exception.diagnostics], ["A42"])
        self.assertIn("t.a66:2:10: error A42", str(ctx.exception))

    def test_listing_shows_errors(self):
        self.assertIn("*** ERROR A42", self.result.listing)


class ImageTest(unittest.TestCase):
    def setUp(self):
        self.result = openc166.assemble(
            "A SECTION CODE AT 100H\n NOP\nA ENDS\n"
            "B SECTION CODE AT 104H\n RET\nB ENDS\n END\n"
        )

    def test_gap_and_fill(self):
        self.assertEqual(self.result.image().data.hex(), "cc00ffffcb00")
        self.assertEqual(self.result.image(fill=0x00).data.hex(), "cc000000cb00")

    def test_base(self):
        image = self.result.image(base=0xFE)
        self.assertEqual(image.base, 0xFE)
        self.assertEqual(image.data.hex(), "ffffcc00ffffcb00")
        with self.assertRaises(ValueError):
            self.result.image(base=0x102)
        with self.assertRaises(ValueError):
            self.result.image(fill=256)
        with self.assertRaises(ValueError):
            self.result.image(base=-2)


class HexTest(unittest.TestCase):
    def setUp(self):
        self.result = openc166.assemble(
            "A SECTION CODE AT 100H\n NOP\nA ENDS\n"
            "B SECTION CODE AT 104H\n RET\nB ENDS\n END\n"
        )

    def test_intel_hex(self):
        self.assertEqual(
            self.result.intel_hex(),
            ":020000040000FA\r\n:02010000CC0031\r\n:02010400CB002E\r\n:00000001FF\r\n",
        )
        with self.assertRaises(ValueError):
            self.result.intel_hex(0)
        with self.assertRaises(ValueError):
            self.result.intel_hex(256)

    def test_srec(self):
        self.assertEqual(
            self.result.srec(),
            "S0030000FC\r\nS1050100CC002D\r\nS1050104CB002A\r\nS5030002FA\r\nS9030000FC\r\n",
        )
        self.assertIn("S206000100CC002C\r\nS206000104CB0029\r\n", self.result.srec(address_size=3))
        with self.assertRaises(ValueError):
            self.result.srec(address_size=5)
        with self.assertRaises(ValueError):
            self.result.srec(record_size=2**40)

    def test_errors_raise(self):
        bad = openc166.assemble(" MOV R1, NOWHERE\n END\n")
        with self.assertRaises(openc166.AssemblyError):
            bad.intel_hex()
        with self.assertRaises(openc166.AssemblyError):
            bad.srec()


class SymbolTest(unittest.TestCase):
    def test_symbols(self):
        r = openc166.assemble(
            "$MOD167\n"
            "$INCLUDE (REG167.INC)\n"
            "Count  EQU 10\n"
            "STEP   SET 2\n"
            "LED    BIT P2.3\n"
            "IDX    EQU R6\n"
            "LOW1   EQU RL1\n"
            "PORT   EQU P2\n"
            "COND   EQU cc_Z\n"
            "GREET  LIT 'MOV R1, #Count'\n"
            "PROG   SECTION CODE AT 0C000H\n"
            "MAIN   PROC FAR\n"
            "       GREET\n"
            "       RET\n"
            "MAIN   ENDP\n"
            "TABLE: DW 1, 2\n"
            "PROG   ENDS\n"
            "       END\n"
        )
        self.assertTrue(r.ok, r.diagnostics)
        s = r.symbols
        self.assertEqual((s["Count"].kind, s["Count"].type, s["Count"].value), ("equ", "number", 10))
        self.assertIs(r.symbol("COUNT"), s["Count"])
        with self.assertRaises(KeyError):
            r.symbol("nothing")
        self.assertEqual((s["STEP"].kind, s["STEP"].value), ("set", 2))
        self.assertEqual((s["LED"].kind, s["LED"].type, s["LED"].value, s["LED"].bit), ("bit", "bit", 0xFFC0, 3))
        self.assertEqual((s["IDX"].type, s["IDX"].value, s["IDX"].bit), ("gpr", 6, None))
        self.assertEqual((s["LOW1"].type, s["LOW1"].value), ("gpr_byte", 2))
        self.assertEqual((s["PORT"].type, s["PORT"].value), ("sfr", 0xFFC0))
        self.assertEqual(s["COND"].type, "cc")
        self.assertEqual((s["GREET"].kind, s["GREET"].type, s["GREET"].value), ("lit", "", None))
        self.assertEqual(s["GREET"].text, "MOV R1, #Count")
        self.assertEqual((s["MAIN"].kind, s["MAIN"].label_type, s["MAIN"].value), ("label", "far", 0xC000))
        self.assertEqual((s["TABLE"].label_type, s["TABLE"].value), ("word", 0xC004))
        self.assertEqual(list(s), sorted(s))


class SectionTest(unittest.TestCase):
    SOURCE = (
        "PROG   SECTION CODE\n"
        "MAIN   PROC NEAR\n"
        "       MOV R4, #COUNT\n"
        "       RET\n"
        "MAIN   ENDP\n"
        "PROG   ENDS\n"
        "Vars   SECTION DATA\n"
        "COUNT: DW 0\n"
        "Vars   ENDS\n"
        "FIXED  SECTION CONST AT 0A000H\n"
        "       DB 1\n"
        "FIXED  ENDS\n"
        "       END\n"
    )

    def test_located(self):
        result = openc166.assemble(self.SOURCE, origin=0x8000)
        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(list(result.sections), ["PROG", "Vars", "FIXED"])
        prog = result.sections["PROG"]
        self.assertEqual(
            prog, openc166.Section("PROG", "CODE", "WORD", 0x8000, 6, "located")
        )
        self.assertEqual(prog.end, 0x8006)
        self.assertEqual(result.sections["Vars"].address, 0x8006)
        self.assertEqual(result.sections["FIXED"].placement, "at")
        self.assertEqual(result.chunks[0].data[:6].hex(), "e6f40680cb00")

    def test_given(self):
        result = openc166.assemble(self.SOURCE, origin=0x8000, sections={"VARS": 0xF600})
        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.sections["Vars"].address, 0xF600)
        self.assertEqual(result.sections["Vars"].placement, "given")
        self.assertEqual(result.chunks[0].data[:6].hex(), "e6f400f6cb00")

    def test_given_problems(self):
        result = openc166.assemble(self.SOURCE, sections={"FIXED": 0x100, "NONE": 0x200})
        self.assertEqual([d.code for d in result.errors], ["OC14"])
        self.assertEqual([d.code for d in result.warnings], ["OC13"])
        for bad in (-1, 0x1000000, "0x100"):
            with self.assertRaises(ValueError):
                openc166.assemble(self.SOURCE, sections={"VARS": bad})
        with self.assertRaises(ValueError):
            openc166.assemble(self.SOURCE, sections={"": 0x100})


class LinkTest(unittest.TestCase):
    MAIN = (
        "EXTERN COUNT:WORD, LIMIT:DATA16\n"
        "PUBLIC MAIN\n"
        "PROG   SECTION CODE 'ROM'\n"
        "MAIN   PROC NEAR\n"
        "       MOV R4, COUNT\n"
        "       MOV R5, #LIMIT\n"
        "       RET\n"
        "MAIN   ENDP\n"
        "PROG   ENDS\n"
        "       END\n"
    )
    DATA = (
        "PUBLIC COUNT, LIMIT\n"
        "LIMIT  EQU 1234H\n"
        "VARS   SECTION DATA AT 0C000H\n"
        "COUNT: DW 0\n"
        "VARS   ENDS\n"
        "       END\n"
    )

    def test_assemble_many(self):
        for sources in ({"main.a66": self.MAIN, "data.a66": self.DATA.encode()},
                        [("main.a66", self.MAIN), ("data.a66", self.DATA)]):
            result = openc166.assemble_many(sources, origin=0x8000)
            self.assertTrue(result.ok, result.diagnostics)
            self.assertEqual(result.modules, ["main.a66", "data.a66"])
            self.assertEqual(result.chunks[0].address, 0x8000)
            self.assertEqual(result.chunks[0].data.hex(), "f2f400c0e6f53412cb00")
            self.assertEqual(result.sections["PROG"].class_name, "ROM")
            self.assertEqual(result.sections["VARS"].class_name, "")

            self.assertEqual(
                [(s.name, s.kind, s.module, s.exported) for s in result.all_symbols],
                [
                    ("COUNT", "extern", "main.a66", False),
                    ("COUNT", "label", "data.a66", True),
                    ("LIMIT", "extern", "main.a66", False),
                    ("LIMIT", "equ", "data.a66", True),
                    ("MAIN", "label", "main.a66", True),
                ],
            )
            count = result.all_symbols[0]
            self.assertEqual((count.value, count.text, count.label_type), (0xC000, "WORD", "word"))
            # symbols holds the PUBLIC definitions.
            self.assertEqual(result.symbol("count").module, "data.a66")
            self.assertEqual(result.symbols["LIMIT"].kind, "equ")

    def test_single_module(self):
        result = openc166.assemble(" NOP\n END\n", file_name="one.a66")
        self.assertEqual(result.modules, ["one.a66"])
        self.assertEqual(result.symbols, {})

    def test_errors(self):
        result = openc166.assemble_many({"main.a66": self.MAIN})
        self.assertEqual([(d.code, d.file, d.line) for d in result.errors],
                         [("OC15", "main.a66", 1), ("OC15", "main.a66", 1)])
        with self.assertRaises(ValueError):
            openc166.assemble_many({})
        with self.assertRaises(TypeError):
            openc166.assemble_many([("a.a66", 5)])

    def test_assemble_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for name, text in (("main.a66", self.MAIN), ("data.a66", self.DATA)):
                paths.append(os.path.join(tmp, name))
                with open(paths[-1], "w") as f:
                    f.write(text)
            result = openc166.assemble_files(paths, origin=0x8000)
            self.assertTrue(result.ok, result.diagnostics)
            self.assertEqual(result.modules, paths)
            self.assertEqual([(c.address, c.data.hex()) for c in result.chunks],
                             [(0x8000, "f2f400c0e6f53412cb00"), (0xC000, "0000")])


class IncludeTest(unittest.TestCase):
    def test_include_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.mkdir(os.path.join(tmp, "src"))
            os.mkdir(os.path.join(tmp, "lib"))
            with open(os.path.join(tmp, "src", "near.inc"), "w") as f:
                f.write("NEARVAL EQU 1\n")
            with open(os.path.join(tmp, "lib", "far.inc"), "w") as f:
                f.write("FARVAL EQU 2\n")
            main = os.path.join(tmp, "src", "main.a66")
            with open(main, "w") as f:
                f.write("$INCLUDE (near.inc)\n$INCLUDE (far.inc)\n DW NEARVAL, FARVAL\n END\n")

            missing = openc166.assemble_file(main)
            self.assertFalse(missing.ok)
            result = openc166.assemble_file(main, include_dirs=[os.path.join(tmp, "lib")])
            self.assertTrue(result.ok, result.diagnostics)
            self.assertEqual(result.image().data.hex(), "01000200")


class ThreadTest(unittest.TestCase):
    def test_parallel(self):
        src = " MOV R4, #0BEEFH\n RET\n END\n"
        results = []

        def work():
            for _ in range(20):
                results.append(openc166.assemble(src).image().data)

        threads = [threading.Thread(target=work) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(set(results), {bytes.fromhex("e6f4efbecb00")})


if __name__ == "__main__":
    unittest.main()
