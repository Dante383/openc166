#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Acceptance test: rebuilds Infineon MiniMon from its TASKING sources.

MiniMon's monitor (MCPROG) and flash drivers (Driver/*) come with their
sources and the Intel HEX files that TASKING's m166, a166, l166 and
ihex166 made of them. This script assembles each source with openc166's
TASKING dialect, puts the sections where the .loc file of the project
puts them, and checks that every byte matches the HEX file.

MiniMon is not part of openc166 (it is copyrighted by Infineon and
Siemens), so give its directory, the one that holds MCPROG and Driver:

    MINIMON_DIR=/path/to/minimon python3 tests/acceptance/minimon.py

or pass the directory as the argument. Without it the script exits with
status 77, which ctest reports as skipped. MiniMon's files are only read:
the sources are copied to a temporary directory first.

The monitor is built four times, once for each pair of HEX files that
comes with it, with the @SET switches in COMM.EQU changed to match.

MiniMon's register files (the STDNAMES controls) came with TASKING, not
with MiniMon. Without them openc166 uses its built-in REG167.INC and says
so (OC32). rstcon.a66 also needs RSTCON, a C161CS/C167CS register that is
not in REG167.INC, so the script gives it a small stand-in file.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIPPED = 77

# MCPROG builds: (KLINE, AUTOBAUD, FULLFEATURES) -> the HEX files.
MONITOR_BUILDS = [
    ((0, 0, 0), ("LOAD.HEX", "MINIMON.HEX")),
    ((1, 0, 0), ("LOADK.HEX", "MINIMONK.HEX")),
    ((0, 0, 1), ("loadf.hex", "minimonf.hex")),
    ((0, 1, 1), ("Loadaf.hex", "minimonaf.hex")),
]


# Stand-ins for missing STDNAMES files: RSTCON is at F1E0H (C167CS Data
# Sheet V2.2, table 7). It is past the extended SFRs that have a short
# address, so TASKING names it with DEFA.
STDNAMES_STANDINS = {
    "reg161cs.def": "$INCLUDE(REG167.INC)\nRSTCON DEFA 0F1E0H\n",
}


def remove_line_after(file_name, label, line):
    """An edit that deletes 'line' where it follows the line 'label'."""

    def edit(directory):
        path = find(directory, file_name)
        text = read_text(path)
        text, n = re.subn(r"(^" + label + r".*\n)" + line + r"[ \t]*\n", r"\1", text, flags=re.M)
        if n != 1:
            raise RuntimeError("{}: cannot find {!r} after {!r}".format(file_name, line, label))
        with open(path, "w", newline="\r\n") as f:
            f.write(text)

    return edit


# Projects whose HEX file is older than the source: the edit gives the
# source the HEX file was made from.
OLD_HEX_FILES = {
    # A29F010.A66 is dated 2000-08-31, A29F010.hex 1999-01-11; the HEX
    # file has no RETS after Cmd3.
    "a29f010.a66": remove_line_after("A29F010.A66", r"Cmd3:", r"\tRETS"),
}


def find(directory, name):
    """The file 'name' in 'directory', in any case (MiniMon comes from DOS)."""
    for entry in os.listdir(directory):
        if entry.lower() == name.lower():
            return os.path.join(directory, entry)
    return None


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("latin-1").replace("\r", "")


def load_hex(path):
    memory = {}
    base = 0
    for line in read_text(path).splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        record = bytes.fromhex(line[1:])
        count, address, kind = record[0], (record[1] << 8) | record[2], record[3]
        data = record[4 : 4 + count]
        if kind == 0:
            for i, b in enumerate(data):
                memory[base + address + i] = b
        elif kind == 2:
            base = ((data[0] << 8) | data[1]) << 4
        elif kind == 4:
            base = ((data[0] << 8) | data[1]) << 16
    return memory


def number(text):
    text = text.strip()
    return int(text[:-1], 16) if text[-1] in "hH" else int(text, 0)


def locate(loc_text, sources):
    """--section options for the AD (SE ...) and AD (RB ...) lines of a .loc file."""
    options = []
    for name, address in re.findall(r"SE\s*\(\s*(\w+)\s*\(\s*(\w+)\s*\)\s*\)", loc_text, re.I):
        options += ["--section", "{}={:#x}".format(name, number(address))]
    rb = re.search(r"RB\s*\(\s*(\w+)\s*\)", loc_text, re.I)
    if rb:
        # The register bank of the module goes there.
        for text in sources:
            for bank in re.findall(r"^[ \t]*(\w+)[ \t]+(?:REGDEF|REGBANK)\b", text, re.I | re.M):
                options += ["--section", "{}={:#x}".format(bank, number(rb.group(1)))]
    return options


def controls(make_text, tool):
    """The controls on the TOOL line of a makefile, after the file name."""
    match = re.search(r"^[ \t]*" + tool + r"[ \t]+\S+[ \t]*(.*)$", make_text, re.I | re.M)
    return match.group(1).split() if match else []


def compare(result_hex, references):
    expected = {}
    for path in references:
        expected.update(load_hex(path))
    actual = load_hex(result_hex)
    wrong = [a for a in sorted(set(expected) | set(actual)) if expected.get(a) != actual.get(a)]
    if not wrong:
        return None
    first = wrong[0]

    def show(memory):
        return "{:02X}".format(memory[first]) if first in memory else "--"

    return "{} byte(s) differ, the first at {:06X}h: expected {}, got {}".format(
        len(wrong), first, show(expected), show(actual)
    )


class Runner:
    def __init__(self, openc166, work):
        self.openc166 = openc166
        self.work = work
        self.failures = 0
        self.count = 0

    def build(self, label, directory, sources, make_text, loc_text, references, edit=None):
        """Assembles copies of 'sources' (file names in 'directory')."""
        self.count += 1
        target = os.path.join(self.work, "{:02d}".format(self.count))
        shutil.copytree(directory, target)
        if edit:
            edit(target)
        texts = [read_text(os.path.join(target, s)) for s in sources]
        options = ["--dialect", "tasking"]
        for c in controls(make_text, "M166") + controls(make_text, "A166"):
            options += ["-c", c]
            stdnames = re.match(r"STDNAMES\((.*)\)$", c, re.I)
            if stdnames and not find(target, stdnames.group(1)):
                standin = STDNAMES_STANDINS.get(stdnames.group(1).lower())
                if standin:
                    with open(os.path.join(target, stdnames.group(1)), "w") as f:
                        f.write(standin)
        options += locate(loc_text, texts)
        out = os.path.join(target, "openc166.hex")
        command = [self.openc166] + options + ["-f", "hex", "-o", out] + sources
        run = subprocess.run(command, cwd=target, capture_output=True, text=True)
        # MiniMon's register files (STDNAMES) are not included with it, so
        # openc166 uses its own and says so (OC32).
        problems = [line for line in (run.stdout + run.stderr).splitlines() if line and "OC32" not in line]
        if run.returncode != 0:
            message = "openc166 failed:\n    " + "\n    ".join(problems)
        else:
            message = compare(out, references)
            if message is None and problems:
                message = "unexpected messages:\n    " + "\n    ".join(problems)
        if message:
            self.failures += 1
            print("FAIL {}: {}".format(label, message))
        else:
            print("ok   {}".format(label))


def set_switches(values):
    def edit(directory):
        path = find(directory, "COMM.EQU")
        text = read_text(path)
        for name, value in zip(("KLINE", "AUTOBAUD", "FULLFEATURES"), values):
            text, n = re.subn(
                r"@SET\s*\(\s*" + name + r"\s*,\s*\d+\s*\)", "@SET ({},{})".format(name, value), text
            )
            if n != 1:
                raise RuntimeError("COMM.EQU: cannot find @SET ({},...)".format(name))
        with open(path, "w", newline="\r\n") as f:
            f.write(text)

    return edit


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("minimon", nargs="?", default=os.environ.get("MINIMON_DIR"))
    parser.add_argument("--openc166", default=shutil.which("openc166") or "openc166")
    args = parser.parse_args()
    if not args.minimon:
        print("skipped: set MINIMON_DIR to the MiniMon directory (the one with MCPROG and Driver)")
        return SKIPPED
    mcprog = find(args.minimon, "MCPROG")
    drivers = find(args.minimon, "Driver")
    if not mcprog or not drivers:
        print("error: no MCPROG and Driver directories in " + args.minimon)
        return 2

    with tempfile.TemporaryDirectory(prefix="openc166-minimon-") as work:
        runner = Runner(os.path.abspath(args.openc166), work)

        make = read_text(find(mcprog, "MAKEFILE"))
        loc = read_text(find(mcprog, "MINIMON.LOC"))
        sources = [os.path.basename(find(mcprog, n)) for n in ("LOAD.A66", "MINIMON.A66")]
        for switches, hex_names in MONITOR_BUILDS:
            references = [find(mcprog, n) for n in hex_names]
            label = "MCPROG {} (KLINE={} AUTOBAUD={} FULLFEATURES={})".format(" + ".join(hex_names), *switches)
            runner.build(label, mcprog, sources, make, loc, references, set_switches(switches))

        projects = [(mcprog, find(mcprog, "rstcon.mak"))]
        for entry in sorted(os.listdir(drivers), key=str.lower):
            directory = os.path.join(drivers, entry)
            if os.path.isdir(directory):
                makes = [f for f in os.listdir(directory) if f.lower().endswith(".mak")]
                if makes:
                    projects.append((directory, os.path.join(directory, makes[0])))
        for directory, make_path in projects:
            make = read_text(make_path)
            app = re.search(r"^APPNAME\s*=\s*(\S+)", make, re.M)
            if not app or not re.search(r"^[ \t]*A166\s", make, re.I | re.M):
                continue  # the XC16x driver is made with another toolchain
            name = app.group(1)
            source, loc, reference = (find(directory, name + ext) for ext in (".a66", ".loc", ".hex"))
            if not (source and loc and reference):
                print("skip {}: missing .a66, .loc or .hex".format(name))
                continue
            label = os.path.relpath(source, args.minimon)
            edit = OLD_HEX_FILES.get(os.path.basename(source).lower())
            if edit:
                label += " (as the older source of the HEX file)"
            runner.build(label, directory, [os.path.basename(source)], make, read_text(loc), [reference], edit)

    print("{} of {} builds match".format(runner.count - runner.failures, runner.count))
    return 1 if runner.failures else 0


if __name__ == "__main__":
    sys.exit(main())
