#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Differential test: assemble sources with Keil A166 and openc166 and compare
every source line.

A line matches when both assemblers agree on whether it has an error and, for
error-free lines, on the address, the code bytes and the warning codes. Code
bytes on error lines are ignored (no binary is produced then). Lines where
only the error codes differ are reported as soft differences with --codes.
The values of all symbols in both symbol tables are compared too.

With --image, sources that both assemblers accept without errors are also
linked with Keil L166 and converted with OH166, and the resulting memory
image is compared byte for byte with the openc166 binary, including which
addresses are left empty.

A source line containing "DIVERGES:" (e.g. "; DIVERGES: reason") marks an
intentional, documented difference from Keil and is not compared.

Keil A166 is not distributed with openc166. Point KEIL_A166 at your own
A166.EXE; on Linux and macOS it runs under Wine (set WINEPREFIX if needed).
Keil's output is only used locally for the comparison and is never stored.

usage: keil_diff.py [--openc166 PATH] [--keil PATH] [--quiet] [--codes] [--image] SOURCE...
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


TIMEOUT = 120  # Keil A166 hangs on some malformed input


class Line:
    def __init__(self):
        self.loc = None
        self.code = ""
        self.diags = []

    def errors(self):
        return sorted(d for d in self.diags if d.endswith("E"))

    def warnings(self):
        return sorted(d for d in self.diags if d.endswith("W"))


def symbol_value(text):
    """'C001H', '0000C001H' or 'FD00H.3' -> (number, bit); None if undefined."""
    m = re.fullmatch(r"([0-9A-F]+)H(?:\.(\d+))?", text)
    if not m:
        return None
    return int(m.group(1), 16), int(m.group(2)) if m.group(2) else None


def parse_keil_listing(text):
    lines = {}
    symbols = {}
    last = None
    in_symbols = False
    row = re.compile(r"^([0-9A-F]{8}|-{8}| {8}) (\S*)\s+(?:[A-Z]\s+)?(\d+) ")
    more = re.compile(r"^[0-9A-F]{8} ([0-9A-F]+)\s*$")
    diag = re.compile(r"^\*\*\* (ERROR|WARNING) #(A\d+) IN \d+ \(.*, LINE (\d+)\)")
    sym = re.compile(r"^([A-Za-z_?@][\w?@]*)[ .]+\s(\S+)\s+(\S+)")
    for l in text.splitlines():
        if l.startswith("N A M E"):
            in_symbols = True
            continue
        if in_symbols:
            m = sym.match(l)
            if m and m.group(2) != "----":
                symbols[m.group(1).upper()] = symbol_value(m.group(3))
            continue
        m = diag.match(l)
        if m:
            lines.setdefault(int(m.group(3)), Line()).diags.append(m.group(2) + m.group(1)[0])
            continue
        m = more.match(l)
        if m and last is not None:
            last.code += m.group(1)
            continue
        m = row.match(l)
        if m:
            last = lines.setdefault(int(m.group(3)), Line())
            if re.fullmatch(r"[0-9A-F]{8}", m.group(1)) and last.loc is None:
                last.loc = int(m.group(1), 16)
            if re.fullmatch(r"[0-9A-F]+", m.group(2)):
                last.code += m.group(2)
    return lines, symbols


def parse_openc166_listing(text):
    lines = {}
    symbols = {}
    last = None
    in_symbols = False
    diag = re.compile(r"^\*\*\* (ERROR|WARNING) (\w+):")
    sym = re.compile(r"^(\S+)\s+(LABEL|EQU|SET|BIT)\s+(?:SFR\s+)?(\S+)")
    for l in text.splitlines():
        if l.startswith("SYMBOLS"):
            in_symbols = True
            continue
        if in_symbols:
            m = sym.match(l)
            if m:
                symbols[m.group(1).upper()] = symbol_value(m.group(3))
            continue
        m = diag.match(l)
        if m:
            if last is not None:
                last.diags.append(m.group(2) + m.group(1)[0])
            continue
        number = l[29:35].strip()
        obj = l[10:28].strip()
        if number.isdigit():
            last = lines.setdefault(int(number), Line())
            if obj and re.fullmatch(r"[0-9A-F]{8}", l[0:8]) and last.loc is None:
                last.loc = int(l[0:8], 16)
            last.code += obj
        elif re.fullmatch(r"[0-9A-F]{8}", l[0:8]) and last is not None:
            last.code += obj
    return lines, symbols


def keil_tool(keil, name, args, workdir):
    cmd = [os.path.join(os.path.dirname(keil), name)] + args
    if os.name != "nt":
        cmd = ["wine"] + cmd
    env = dict(os.environ, WINEDEBUG="-all")
    p = subprocess.run(cmd, cwd=workdir, env=env, capture_output=True, timeout=TIMEOUT)
    return p.stdout.decode("latin-1") + p.stderr.decode("latin-1")


def run_keil(keil, source, workdir):
    shutil.copy(source, os.path.join(workdir, "t.a66"))
    keil_tool(keil, os.path.basename(keil), ["t.a66"], workdir)
    with open(os.path.join(workdir, "t.LST"), encoding="latin-1") as f:
        return parse_keil_listing(f.read())


def run_openc166(tool, source, workdir):
    lst = os.path.join(workdir, "o.lst")
    subprocess.run([tool, source, "-o", os.path.join(workdir, "o.bin"), "-l", lst],
                   capture_output=True, timeout=TIMEOUT)
    with open(lst, encoding="utf-8") as f:
        return parse_openc166_listing(f.read())


def read_hex86(path):
    """Intel HEX (as written by OH166) -> {address: byte}."""
    data = {}
    base = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            b = bytes.fromhex(line[1:])
            n, addr, kind = b[0], (b[1] << 8) | b[2], b[3]
            if kind == 0:
                for i in range(n):
                    data[base + addr + i] = b[4 + i]
            elif kind == 2:
                base = ((b[4] << 8) | b[5]) << 4
            elif kind == 4:
                base = ((b[4] << 8) | b[5]) << 16
    return data


def openc166_image(tool, source, workdir):
    """openc166 output as {address: byte}, without the gaps. Assembling twice
    with different fill bytes tells written bytes from gaps."""
    images = []
    for fill in ("0", "0FFH"):
        out = os.path.join(workdir, "img.bin")
        p = subprocess.run([tool, source, "-o", out, "--base", "0", "--fill", fill],
                           capture_output=True, timeout=TIMEOUT)
        if p.returncode != 0:
            return None
        with open(out, "rb") as f:
            images.append(f.read())
    a, b = images
    return {i: a[i] for i in range(len(a)) if a[i] == b[i]}


def compare_image(source, keil, tool, workdir):
    """Link with Keil and compare memory images. Returns a list of problems,
    or None when the Keil evaluation linker refuses the code size."""
    if "L250" in keil_tool(keil, "L166.EXE", ["t.OBJ", "TO", "t.ABS"], workdir):
        return None
    keil_tool(keil, "OH166.EXE", ["t.ABS"], workdir)
    path = os.path.join(workdir, "t.H86")
    if not os.path.exists(path):
        return ["Keil L166/OH166 produced no image"]
    k = read_hex86(path)
    o = openc166_image(tool, source, workdir)
    if o is None:
        return ["openc166 produced no image"]
    problems = []
    for addr in sorted(set(k) | set(o)):
        kb, ob = k.get(addr), o.get(addr)
        if kb != ob:
            fmt = lambda v: "--" if v is None else f"{v:02X}"
            problems.append(f"{addr:06X}: keil {fmt(kb)} openc166 {fmt(ob)}")
    return problems


def classify(kl, ol, shift):
    """Return 'ok', 'soft' (only error codes differ) or 'hard'. 'shift' is the
    expected difference between the Keil and openc166 addresses."""
    if bool(kl.errors()) != bool(ol.errors()):
        return "hard"
    if kl.errors():
        return "ok" if kl.errors() == ol.errors() else "soft"
    if shift is not None and kl.loc is not None and ol.loc is not None and kl.loc - ol.loc != shift:
        return "hard"
    return "ok" if (kl.code, kl.warnings()) == (ol.code, ol.warnings()) else "hard"


def definitions(text):
    """{symbol name: [numbers of the lines defining it]}"""
    defs = {}
    for n, l in enumerate(text, 1):
        m = re.match(r"\s*([A-Za-z_?@][\w?@]*)\s*(?::|\s+(?:EQU|SET|BIT|LABEL|PROC|DEFR|DEFA|DEFB)\b)", l, re.I)
        if m:
            defs.setdefault(m.group(1).upper(), []).append(n)
    return defs


def fmt_loc(line):
    return "" if line.loc is None else f"{line.loc:06X} "


def compare(source, keil, tool, quiet=False, codes=False, image=False):
    with open(source, encoding="latin-1") as f:
        text = f.read().splitlines()
    counts = {"ok": 0, "soft": 0, "hard": 0, "diverges": 0, "symbols": 0, "image": None}
    with tempfile.TemporaryDirectory() as work:
        k, ksym = run_keil(keil, source, work)
        o, osym = run_openc166(tool, source, work)
        clean = not any(l.errors() for l in k.values()) and not any(l.errors() for l in o.values())
        if image and clean:
            problems = compare_image(source, keil, tool, work)
            counts["image"] = "skipped" if problems is None else not problems
            if problems is not None:
                counts["ok" if not problems else "hard"] += 1
            if problems and not quiet:
                print(f"{os.path.basename(source)}: memory images differ")
                for p in problems[:20]:
                    print("    " + p)
    shift = 0
    shift_at = {}
    for n in sorted(set(k) | set(o)):
        src = text[n - 1].strip() if 0 < n <= len(text) else ""
        if "DIVERGES:" in src:
            counts["diverges"] += 1
            shift_at[n] = shift = None
            continue
        kl, ol = k.get(n, Line()), o.get(n, Line())
        shift_at[n] = shift
        kind = classify(kl, ol, shift)
        counts[kind] += 1
        # Keil may emit code where openc166 reports an error (or the other way
        # round), which moves every later address; compare the following
        # addresses relative to the first line after such a line.
        if kl.errors() or ol.errors():
            shift = None
        elif shift is None and kl.loc is not None and ol.loc is not None:
            shift = kl.loc - ol.loc
        if kind == "hard" and not quiet or kind == "soft" and codes:
            print(f"{os.path.basename(source)}:{n}: {src}" + ("  [codes only]" if kind == "soft" else ""))
            print(f"    keil:     {fmt_loc(kl)}{kl.code or '-':16} {' '.join(sorted(kl.diags))}")
            print(f"    openc166: {fmt_loc(ol)}{ol.code or '-':16} {' '.join(sorted(ol.diags))}")
    # Symbols are compared like lines: skipped when a defining line has an
    # error or a documented divergence, and addresses allow for the shift
    # caused by earlier divergences.
    defs = definitions(text)
    for name in sorted(set(ksym) & set(osym)):
        kv, ov = ksym[name], osym[name]
        lines = defs.get(name, [])
        if kv is None or ov is None or not lines:
            continue
        if any(shift_at.get(n, 0) is None or k.get(n, Line()).errors() or o.get(n, Line()).errors()
               or "DIVERGES:" in text[n - 1] for n in lines):
            continue
        sh = shift_at.get(lines[0], 0)
        same = kv == ov or (kv[1] == ov[1] and kv[0] - ov[0] == sh)
        counts["symbols"] += 1
        counts["ok" if same else "hard"] += 1
        if not same:
            if not quiet:
                print(f"{os.path.basename(source)}: symbol {name}: keil {ksym[name]} openc166 {osym[name]}")
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--keil", default=os.environ.get("KEIL_A166"), help="path to A166.EXE (default: $KEIL_A166)")
    ap.add_argument("--openc166", default=os.environ.get("OPENC166", os.path.join(ROOT, "build", "cli", "openc166")))
    ap.add_argument("--quiet", action="store_true", help="only print totals")
    ap.add_argument("--codes", action="store_true", help="also list lines whose error codes differ")
    ap.add_argument("--image", action="store_true", help="also link with Keil and compare memory images")
    args = ap.parse_args()
    if not args.keil:
        sys.exit("keil_diff.py: set KEIL_A166 or pass --keil")
    total_bad = 0
    for s in args.sources:
        c = compare(s, args.keil, args.openc166, args.quiet, args.codes, args.image)
        img = {None: "", True: ", image identical", False: ", IMAGE DIFFERS",
               "skipped": ", image skipped: Keil evaluation size limit"}[c["image"]]
        print(f"{s}: {c['ok'] + c['soft']}/{c['ok'] + c['soft'] + c['hard']} checks pass"
              f" ({c['symbols']} symbols; {c['soft']} lines with different error codes,"
              f" {c['diverges']} documented divergences{img})")
        total_bad += c["hard"]
    sys.exit(1 if total_bad else 0)


if __name__ == "__main__":
    main()
