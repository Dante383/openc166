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
addresses are left empty. The messages L166 gives are compared with the
openc166 codes for the same problems (OC15..OC22, see LINK_CODES); A166
cannot report those, so they are not compared line by line.

Sources may use $INCLUDE and $INCDIR. Lines are numbered across all files
as in Keil's listing; both assemblers run in the directory of the source.

A source line containing "DIVERGES:" (e.g. "; DIVERGES: reason") marks an
intentional, documented difference from Keil and is not compared.

A directory instead of a source is a link case: its .a66 files, in order of
name, are modules of one program. Keil assembles each with A166 and links
them with L166; openc166 gets them all on one command line. The error and
warning codes of both are compared (L166 codes as the openc166 codes that
stand for them, such as L121 as OC18), and, if there are no errors, the
memory images.

Keil A166 is not distributed with openc166. Point KEIL_A166 at your own
A166.EXE; on Linux and macOS it runs under Wine (set WINEPREFIX if needed).
Keil's output is only used locally for the comparison and is never stored.

usage: keil_diff.py [--openc166 PATH] [--keil PATH] [--quiet] [--codes] [--image] SOURCE|DIR...
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
        self.skipped = False  # in a false IF block: listed without a number

    def errors(self):
        # Keil may report one problem more than once (in DW, A63 at both the
        # operand and the operator); only the distinct codes count.
        return sorted({d for d in self.diags if d.endswith("E")})

    def warnings(self):
        return sorted(d for d in self.diags if d.endswith("W"))


def symbol_value(text):
    """'C001H', '0000C001H' or 'FD00H.3' -> (number, bit); None if undefined."""
    m = re.fullmatch(r"([0-9A-F]+)H(?:\.(\d+))?", text)
    if not m:
        return None
    return int(m.group(1), 16), int(m.group(2)) if m.group(2) else None


def locate(source, number, text, wrap=None):
    """Number of the first line after 'number' with the source 'text'. Lines
    in false IF blocks have no number in the listings, and $NOCOND leaves
    some of them out. Keil expands tabs and wraps lines after 'wrap'
    characters."""
    text = text.rstrip()
    for n in range(number + 1, len(source) + 1):
        src = source[n - 1].rstrip()
        if wrap:
            src = src.expandtabs(8)
        if src == text or (wrap and len(text) >= wrap and src.startswith(text)):
            return n
    return number + 1


def parse_keil_listing(text, source=()):
    """'source' holds the expanded source lines; it tells the rows of lines
    in false IF blocks (listed without a number) from long lines that Keil
    wraps onto extra rows."""
    lines = {}
    symbols = {}
    last = None
    in_symbols = False
    body = False
    number = 0  # last line listed
    printed = 0  # characters of its source printed so far
    # Lines from include files carry a nesting marker (+1, +2, ...).
    row = re.compile(r"^([0-9A-F]{8}|-{8}| {8}) (\S*)\s+(?:[A-Z]\s+)?(?:\+\d+\s+)?(\d+) ")
    # The linker fills in the digits that Keil lists as '?' (marked R).
    more = re.compile(r"^[0-9A-F]{8} ([0-9A-F?]+)(?:\s+R)?\s*$")
    unnumbered = re.compile(r" *(?:\+\d+)? *")
    # "IN n" is the listing line; Keil's "LINE n" within a file is unreliable.
    diag = re.compile(r"^\*\*\* (ERROR|WARNING) #(A\d+) IN (\d+) \(")
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
        if l.startswith("LOC ") and "SOURCE" in l:
            body = True
            continue
        if l.startswith("SYMBOL TABLE"):
            body = False
        m = diag.match(l)
        if m:
            # Keil numbers the problems it finds at the end of the file one
            # line before the last line.
            at = int(m.group(3)) + (m.group(2) in ("A12", "A89"))
            lines.setdefault(at, Line()).diags.append(m.group(2) + m.group(1)[0])
            continue
        m = more.match(l)
        if m and last is not None:
            last.code += m.group(1)
            continue
        if body and len(l) >= 34 and unnumbered.fullmatch(l[:34]):
            want = len(source[number - 1].rstrip().expandtabs(8)) if 0 < number <= len(source) else 0
            if printed < want:  # the rest of a long line
                printed += len(l) - 34
                continue
            number = locate(source, number, l[34:], wrap=86)
            last = lines.setdefault(number, Line())
            last.skipped = True
            printed = len(l) - 34
            continue
        m = row.match(l)
        if m:
            number = int(m.group(3))
            printed = len(l) - 34
            last = lines.setdefault(number, Line())
            if re.fullmatch(r"[0-9A-F]{8}", m.group(1)) and last.loc is None:
                last.loc = int(m.group(1), 16)
            if re.fullmatch(r"[0-9A-F?]+", m.group(2)):
                last.code += m.group(2)
    return lines, symbols


SECTION_START = re.compile(r"\s*([A-Za-z_?@][\w?@]*)\s+SECTION\b", re.I)
SECTION_END = re.compile(r"\s*([A-Za-z_?@][\w?@]*)\s+ENDS\b", re.I)


def relocatable_bases(source, sections):
    """{line number: address} for the lines inside sections that openc166
    placed itself (no AT). Keil lists offsets from the start of such
    sections, openc166 the final addresses."""
    bases = {}
    stack = []
    for n, l in enumerate(source, 1):
        code = l.split(";")[0]
        m = SECTION_START.match(code)
        if m:
            stack.append(m.group(1).upper())
            continue
        m = SECTION_END.match(code)
        if m:
            if stack and stack[-1] == m.group(1).upper():
                stack.pop()
            continue
        if stack and stack[-1] in sections and sections[stack[-1]][1] != "AT":
            bases[n] = sections[stack[-1]][0]
    return bases


def parse_openc166_listing(text, source=()):
    lines = {}
    symbols = {}
    labels = set()
    sections = {}
    last = None
    in_symbols = in_sections = False
    diag = re.compile(r"^\*\*\* (ERROR|WARNING) (\w+):")
    sym = re.compile(r"^(\S+)\s+(LABEL|EQU|SET|BIT)\s+(?:SFR\s+)?(\S+)")
    sec = re.compile(r"^(\S+)\s+[A-Z]+\s+[A-Z]+\s+([0-9A-F]+)H\s+size [0-9A-F]+H\s+(\S+)")
    for l in text.splitlines():
        if l == "SYMBOLS":
            in_symbols = True
            continue
        if l == "SECTIONS":
            in_symbols, in_sections = False, True
            continue
        if in_sections:
            m = sec.match(l)
            if m:
                sections[m.group(1).upper()] = (int(m.group(2), 16), m.group(3))
            continue
        if in_symbols:
            m = sym.match(l)
            if m:
                symbols[m.group(1).upper()] = symbol_value(m.group(3))
                if m.group(2) == "LABEL":
                    labels.add(m.group(1).upper())
            continue
        m = diag.match(l)
        if m:
            if last is not None:
                last.diags.append(m.group(2) + m.group(1)[0])
            continue
        number = l[29:35].strip()
        obj = l[10:26].strip()
        if number.isdigit():
            last = lines.setdefault(int(number), Line())
            if obj and re.fullmatch(r"[0-9A-F]{8}", l[0:8]) and last.loc is None:
                last.loc = int(l[0:8], 16)
            last.code += obj
        elif re.fullmatch(r"[0-9A-F]{8}", l[0:8]) and last is not None:
            last.code += obj
        elif len(l) >= 37 and not l[:27].strip() and not number and re.fullmatch(r"(\+\d)? *", l[27:29]):
            # a line in a false IF block, listed without a number
            last = lines.setdefault(locate(source, max(lines, default=0), l[37:]), Line())
            last.skipped = True
    bases = relocatable_bases(source, sections)
    for n, line in lines.items():
        if line.loc is not None:
            line.loc -= bases.get(n, 0)
    defs = definitions(source)
    for name in labels:
        v, where = symbols.get(name), defs.get(name)
        if v is not None and where and where[0] in bases:
            symbols[name] = (v[0] - bases[where[0]], v[1])
    return lines, symbols


def keil_tool(keil, name, args, workdir):
    cmd = [os.path.join(os.path.dirname(keil), name)] + args
    if os.name != "nt":
        cmd = ["wine"] + cmd
    env = dict(os.environ, WINEDEBUG="-all")
    p = subprocess.run(cmd, cwd=workdir, env=env, capture_output=True, timeout=TIMEOUT)
    return p.stdout.decode("latin-1") + p.stderr.decode("latin-1")


INCLUDE = re.compile(r"\$\s*(?:INCLUDE|IC)\s*\(([^)]*)\)\s*(?:;.*)?$", re.I)
INCDIR = re.compile(r"\b(?:INCDIR|ID)\s*\(([^)]*)\)", re.I)


def find_file(directory, name):
    """Path of 'name' below 'directory', ignoring case as Windows does."""
    path = directory
    for part in name.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if not os.path.exists(os.path.join(path, part)):
            match = [e for e in os.listdir(path) if e.lower() == part.lower()] if os.path.isdir(path) else []
            if not match:
                return None
            part = match[0]
        path = os.path.join(path, part)
    return path if os.path.isfile(path) else None


def expand(source):
    """Source lines with include files expanded in place, so that line n is
    line n of the listings. Like Keil, include files are searched in the
    source directory and then in $INCDIR directories."""
    root = os.path.dirname(os.path.abspath(source))
    dirs = [root]
    out = []
    carry = []  # last line of an include file without a final newline

    def read(path, depth):
        with open(path, encoding="latin-1") as f:
            data = f.read()
        lines = data.splitlines()
        for i, l in enumerate(lines):
            # Keil joins it to the next line read.
            if depth > 0 and i == len(lines) - 1 and not data.endswith("\n"):
                carry.append(l)
                continue
            l = "".join(carry) + l
            carry.clear()
            out.append(l)
            if l.startswith("$"):
                for d in INCDIR.findall(l):
                    dirs.extend(os.path.join(root, x.strip()) for x in d.split(";") if x.strip())
            m = INCLUDE.match(l)
            if m and depth < 32:
                for d in dirs:
                    found = find_file(d, m.group(1).strip())
                    if found:
                        read(found, depth + 1)
                        break

    read(source, 0)
    if carry:
        out.append("".join(carry))
    return out


def run_keil(keil, source, workdir):
    # Include files are looked up relative to the source, so the whole source
    # directory goes along.
    if any(INCLUDE.match(l) for l in expand(source)):
        shutil.copytree(os.path.dirname(os.path.abspath(source)), workdir, dirs_exist_ok=True)
    shutil.copy(source, os.path.join(workdir, "t.a66"))
    keil_tool(keil, os.path.basename(keil), ["t.a66"], workdir)
    with open(os.path.join(workdir, "t.LST"), encoding="latin-1") as f:
        return parse_keil_listing(f.read(), expand(source))


def run_openc166(tool, source, workdir):
    lst = os.path.join(workdir, "o.lst")
    subprocess.run([os.path.abspath(tool), os.path.abspath(source), "-o", os.path.join(workdir, "o.bin"), "-l", lst],
                   capture_output=True, timeout=TIMEOUT, cwd=os.path.dirname(os.path.abspath(source)))
    with open(lst, encoding="utf-8") as f:
        return parse_openc166_listing(f.read(), expand(source))


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
            if len(b) != n + 5 or sum(b) & 0xFF:
                raise ValueError(f"{path}: bad record {line}")
            if kind == 0:
                for i in range(n):
                    data[base + addr + i] = b[4 + i]
            elif kind == 2:
                base = ((b[4] << 8) | b[5]) << 4
            elif kind == 4:
                base = ((b[4] << 8) | b[5]) << 16
    return data


def openc166_image(tool, source, workdir):
    """openc166 output as {address: byte}, read from its Intel HEX file."""
    out = os.path.join(workdir, "img.hex")
    p = subprocess.run([os.path.abspath(tool), os.path.abspath(source), "-o", out],
                       capture_output=True, timeout=TIMEOUT, cwd=os.path.dirname(os.path.abspath(source)))
    if p.returncode != 0:
        return None
    return read_hex86(out)


def link_messages(text):
    """The openc166 codes for the L166 messages in 'text'."""
    codes = []
    for _, c in KEIL_MESSAGE.findall(text):
        mapped = LINK_CODES.get(c, c) if c.startswith("L") else c
        if mapped:
            codes.append(mapped)
    return codes


def compare_image(source, keil, tool, workdir, link_codes=()):
    """Link with Keil and compare memory images, and the messages of the link
    stage (L166) with 'link_codes', those openc166 gave. Returns a list of
    problems, "link errors" when both fail to link in the same way, or None
    when the Keil evaluation linker refuses the code size."""
    linked = keil_tool(keil, "L166.EXE", ["t.OBJ", "TO", "t.ABS"], workdir)
    if "L250" in linked:
        return None
    kcodes = link_messages(linked)
    if sorted(kcodes) != sorted(link_codes):
        return [f"link codes: keil {' '.join(sorted(kcodes)) or '-'}  openc166 {' '.join(sorted(link_codes)) or '-'}"]
    if re.search(r"\*\*\* (FATAL )?ERROR", linked):
        return "link errors"
    keil_tool(keil, "OH166.EXE", ["t.ABS"], workdir)
    path = find_file(workdir, "t.H86")
    if path is None:
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


# L166 codes and the openc166 codes for the same problem. None marks follow-on
# errors that openc166 does not repeat: L118 and L128 at each reference to a
# duplicate or missing external, L120 for content of a section that failed.
LINK_CODES = {"L15": "OC20", "L21": "OC17", "L101": "OC19", "L104": "OC16", "L121": "OC18",
              "L127": "OC15", "L129": "OC21", "L112": "OC22", "L115": "OC22", "L116": "OC22", "L118": None,
              "L119": None, "L120": None, "L128": None}
LINK_ONLY = {c for c in LINK_CODES.values() if c}
KEIL_MESSAGE = re.compile(r"\*\*\* (ERROR|WARNING) #?([AL]\d+)")
OPENC166_MESSAGE = re.compile(r"\b(error|warning) ([A-Z]+\d+):")


def compare_link(directory, keil, tool, quiet=False):
    """A link case: the diagnostic codes, and the images if both succeed."""
    modules = sorted(f for f in os.listdir(directory) if f.lower().endswith(".a66"))
    name = os.path.basename(os.path.normpath(directory))
    problems = []
    with tempfile.TemporaryDirectory() as work:
        shutil.copytree(directory, work, dirs_exist_ok=True)
        kcodes = []
        for i, m in enumerate(modules):
            # Short names, as Keil's tools want 8.3 names.
            shutil.copy(os.path.join(directory, m), os.path.join(work, f"m{i}.a66"))
            keil_tool(keil, os.path.basename(keil), [f"m{i}.a66"], work)
            listing = find_file(work, f"m{i}.LST")
            if listing is None:
                problems.append(f"A166 produced no listing for {m}")
                continue
            with open(listing, encoding="latin-1") as f:
                kcodes += [c for _, c in KEIL_MESSAGE.findall(f.read())]
        objs = ",".join(f"m{i}.OBJ" for i in range(len(modules)))
        linked = keil_tool(keil, "L166.EXE", [objs, "TO", "t.ABS"], work)
        if "L250" in linked:
            return None
        kcodes += link_messages(linked)
        out = os.path.join(work, "img.hex")
        p = subprocess.run([os.path.abspath(tool), *modules, "-o", out], capture_output=True, timeout=TIMEOUT,
                           cwd=os.path.abspath(directory))
        ocodes = [c for _, c in OPENC166_MESSAGE.findall(p.stdout.decode() + p.stderr.decode())]
        if sorted(kcodes) != sorted(ocodes):
            problems.append(f"codes: keil {' '.join(sorted(kcodes)) or '-'}"
                            f"  openc166 {' '.join(sorted(ocodes)) or '-'}")
        keil_failed = bool(re.search(r"\*\*\* (FATAL )?ERROR", linked)) or find_file(work, "t.ABS") is None
        if not keil_failed and p.returncode == 0:
            keil_tool(keil, "OH166.EXE", ["t.ABS"], work)
            hex86 = find_file(work, "t.H86")
            if hex86 is None:
                problems.append("Keil L166/OH166 produced no image")
                hex86 = os.devnull
            k, o = read_hex86(hex86), read_hex86(out)
            for addr in sorted(set(k) | set(o)):
                kb, ob = k.get(addr), o.get(addr)
                if kb != ob:
                    fmt = lambda v: "--" if v is None else f"{v:02X}"
                    problems.append(f"{addr:06X}: keil {fmt(kb)} openc166 {fmt(ob)}")
        elif keil_failed != (p.returncode != 0):
            problems.append("only " + ("Keil" if keil_failed else "openc166") + " fails")
    if problems and not quiet:
        print(f"{name}: link results differ")
        for line in problems[:20]:
            print("    " + line)
    return problems


def classify(kl, ol, shift):
    """Return 'ok', 'soft' (only error codes differ) or 'hard'. 'shift' is the
    expected difference between the Keil and openc166 addresses."""
    if kl.skipped != ol.skipped:
        return "hard"
    if bool(kl.errors()) != bool(ol.errors()):
        return "hard"
    if kl.errors():
        return "ok" if kl.errors() == ol.errors() else "soft"
    if shift is not None and kl.loc is not None and ol.loc is not None and kl.loc - ol.loc != shift:
        return "hard"
    return "ok" if same_code(kl.code, ol.code) and kl.warnings() == ol.warnings() else "hard"


def same_code(keil, tool):
    """True if the code matches, with '?' in Keil's code matching any digit."""
    return len(keil) == len(tool) and all(k in ("?", t) for k, t in zip(keil, tool))


def definitions(text):
    """{symbol name: [numbers of the lines defining it]}"""
    defs = {}
    for n, l in enumerate(text, 1):
        m = re.match(r"\s*([A-Za-z_?@][\w?@]*)\s*(?::|\s+(?:EQU|SET|BIT|LABEL|PROC|DEFR|DEFA|DEFB)\b)", l, re.I)
        if m:
            defs.setdefault(m.group(1).upper(), []).append(n)
    return defs


def fmt_code(line):
    return line.code or ("(skipped)" if line.skipped else "-")


def fmt_loc(line):
    return "" if line.loc is None else f"{line.loc:06X} "


def compare(source, keil, tool, quiet=False, codes=False, image=False):
    text = expand(source)
    counts = {"ok": 0, "soft": 0, "hard": 0, "diverges": 0, "symbols": 0, "image": None}
    with tempfile.TemporaryDirectory() as work:
        k, ksym = run_keil(keil, source, work)
        o, osym = run_openc166(tool, source, work)
        # A166 cannot see what L166 reports (OC15..OC21 in openc166); those
        # are checked against L166 when the image is, and not line by line.
        link_codes = []
        for line in o.values():
            link_codes += [d[:-1] for d in line.diags if d[:-1] in LINK_ONLY]
            line.diags = [d for d in line.diags if d[:-1] not in LINK_ONLY]
        clean = not any(l.errors() for l in k.values()) and not any(l.errors() for l in o.values())
        if image and clean:
            problems = compare_image(source, keil, tool, work, link_codes)
            if problems == "link errors":
                counts["image"], problems = problems, []
            else:
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
            print(f"    keil:     {fmt_loc(kl)}{fmt_code(kl):16} {' '.join(sorted(kl.diags))}")
            print(f"    openc166: {fmt_loc(ol)}{fmt_code(ol):16} {' '.join(sorted(ol.diags))}")
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
        if os.path.isdir(s):
            problems = compare_link(s, args.keil, args.openc166, args.quiet)
            status = ("skipped: Keil evaluation size limit" if problems is None
                      else "codes and image identical" if not problems else "LINK RESULTS DIFFER")
            count = sum(f.lower().endswith(".a66") for f in os.listdir(s))
            print(f"{s}: {count} modules linked, {status}")
            total_bad += bool(problems)
            continue
        c = compare(s, args.keil, args.openc166, args.quiet, args.codes, args.image)
        img = {None: "", True: ", image identical", False: ", IMAGE DIFFERS",
               "skipped": ", image skipped: Keil evaluation size limit",
               "link errors": ", same link errors"}[c["image"]]
        print(f"{s}: {c['ok'] + c['soft']}/{c['ok'] + c['soft'] + c['hard']} checks pass"
              f" ({c['symbols']} symbols; {c['soft']} lines with different error codes,"
              f" {c['diverges']} documented divergences{img})")
        total_bad += c["hard"]
    sys.exit(1 if total_bad else 0)


if __name__ == "__main__":
    main()
