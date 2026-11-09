# SPDX-License-Identifier: MIT
"""ELF output with DWARF debug information."""

import glob
import os
import struct
import tempfile
import unittest

import openc166

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PROGRAM = """$MOD167
TWICE   MACRO   R
        ADD     R, #1
        ADD     R, #1
        ENDM
PUBLIC  START
CODE0   SECTION CODE AT 0E000H
START:  MOV     R4, #5
        TWICE   R4
        CALLR   INCFN
DONE:   JMPR    cc_UC, DONE
$INCLUDE(inc.inc)
CODE0   ENDS
DATA0   SECTION DATA AT 0F600H
COUNT:  DW      1234H
DATA0   ENDS
        END
"""


class Elf:
    """Just enough of an ELF32 reader for the tests."""

    def __init__(self, data):
        self.data = data
        assert data[:4] == b"\x7fELF"
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, _, _, self.phentsize, self.phnum,
         self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        self.sections = []
        for i in range(self.shnum):
            self.sections.append(struct.unpack_from("<10I", data, self.shoff + 40 * i))
        names = self.sections[self.shstrndx]
        self.names = [self._str(names[4], s[0]) for s in self.sections]
        self.segments = [struct.unpack_from("<8I", data, self.phoff + 32 * i) for i in range(self.phnum)]

    def _str(self, table_offset, at):
        end = self.data.index(b"\0", table_offset + at)
        return self.data[table_offset + at:end].decode()

    def section(self, name):
        s = self.sections[self.names.index(name)]
        return self.data[s[4]:s[4] + s[5]]

    def symbols(self):
        sym = self.sections[self.names.index(".symtab")]
        strtab = self.sections[sym[6]][4]
        out = {}
        for off in range(sym[4], sym[4] + sym[5], 16):
            name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", self.data, off)
            if name:
                out[self._str(strtab, name)] = (value, size, info >> 4, info & 15)
        return out

    def image(self):
        mem = {}
        for p_type, offset, vaddr, _, filesz, _, _, _ in self.segments:
            self.assertion = p_type == 1
            for i in range(filesz):
                mem[vaddr + i] = self.data[offset + i]
        return mem


def uleb(data, pos):
    v = shift = 0
    while True:
        b = data[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, pos


def sleb(data, pos):
    v = shift = 0
    while True:
        b = data[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                v -= 1 << shift
            return v, pos


def line_table(data):
    """Decodes a DWARF 4 .debug_line section into (address, file name, line)."""
    rows = []
    pos = 0
    while pos < len(data):
        length = struct.unpack_from("<I", data, pos)[0]
        unit_end = pos + 4 + length
        version, header_length = struct.unpack_from("<HI", data, pos + 4)
        assert version == 4
        prog = pos + 10 + header_length
        p = pos + 10
        _, _, _, line_base, line_range, opcode_base = struct.unpack_from("<BBBbBB", data, p)
        p += 6 + opcode_base - 1
        while data[p]:
            p = data.index(b"\0", p) + 1
        p += 1
        files = []
        while data[p]:
            end = data.index(b"\0", p)
            files.append(data[p:end].decode())
            p = end + 1
            for _ in range(3):
                _, p = uleb(data, p)
        p = prog
        addr, file, line = 0, 1, 1
        while p < unit_end:
            op = data[p]
            p += 1
            if op == 0:
                n, p = uleb(data, p)
                sub = data[p]
                if sub == 1:
                    addr, file, line = 0, 1, 1
                elif sub == 2:
                    addr = struct.unpack_from("<I", data, p + 1)[0]
                p += n
            elif op == 1:
                rows.append((addr, files[file - 1], line))
            elif op == 2:
                n, p = uleb(data, p)
                addr += n
            elif op == 3:
                n, p = sleb(data, p)
                line += n
            elif op == 4:
                file, p = uleb(data, p)
            else:
                raise AssertionError("unexpected opcode {}".format(op))
        pos = unit_end
    return rows


class ElfTest(unittest.TestCase):
    def assemble(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "inc.inc"), "w") as f:
            f.write("INCFN:  MOV     R5, #2\n        RET\n")
        r = openc166.assemble(PROGRAM, file_name=os.path.join(d, "prog.a66"))
        self.assertTrue(r.ok, r.errors)
        return r

    def test_header_segments_and_symbols(self):
        r = self.assemble()
        elf = Elf(r.elf())
        self.assertEqual(elf.machine, 116)  # EM_C166
        self.assertEqual(elf.type, 2)
        self.assertEqual([n for n in elf.names if n in ("CODE0", "DATA0")], ["CODE0", "DATA0"])
        image = r.image()
        mem = elf.image()
        for c in r.chunks:
            for i, b in enumerate(c.data):
                self.assertEqual(mem[c.address + i], b)
        syms = elf.symbols()
        self.assertEqual(syms["START"][0], 0xE000)
        self.assertEqual(syms["START"][2], 1)  # global
        self.assertEqual(syms["COUNT"], (0xF600, 2, 0, 1))  # local object of 2 bytes
        self.assertEqual(syms["INCFN"][0], r.symbol("INCFN").value)
        del image

    def test_line_table(self):
        r = self.assemble()
        rows = line_table(Elf(r.elf()).section(".debug_line"))
        by_addr = {a: (os.path.basename(f), l) for a, f, l in rows}
        self.assertEqual(by_addr[0xE000], ("prog.a66", 8))
        self.assertEqual(by_addr[0xE002], ("prog.a66", 3))  # the macro body
        self.assertEqual(by_addr[0xE004], ("prog.a66", 4))
        self.assertEqual(by_addr[r.symbol("INCFN").value], ("inc.inc", 1))
        self.assertEqual(by_addr[0xF600], ("prog.a66", 15))

    def test_examples(self):
        for path in sorted(glob.glob(os.path.join(ROOT, "examples", "**", "*.a66"), recursive=True)):
            with self.subTest(path=os.path.relpath(path, ROOT)):
                r = openc166.assemble_file(path, include_dirs=[os.path.dirname(path)])
                if not r.ok:
                    continue
                elf = Elf(r.elf())
                mem = elf.image()
                for c in r.chunks:
                    self.assertEqual(bytes(mem[c.address + i] for i in range(len(c.data))), c.data)
                covered = {a for a, _, _ in line_table(elf.section(".debug_line"))}
                info = r.debug_info()
                for l in info["lines"]:
                    self.assertIn(l["address"], covered)


if __name__ == "__main__":
    unittest.main()
