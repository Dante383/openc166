#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates an instruction sweep from the instruction table (isa.json): every
hardware instruction form with sample operands of each operand class.

usage: gen_sweep.py ISA_JSON {c166|c167} > sweep.a66
"""
import itertools
import json
import sys

SAMPLES = {
    "Rw": ["R0", "R7", "R15"],
    "Rb": ["RL0", "RH3", "RH7"],
    "[Rw]": ["[R0]", "[R15]"],
    "[Rw+]": ["[R1+]", "[R14+]"],
    "[-Rw]": ["[-R0]", "[-R13]"],
    "[Ri]": ["[R0]", "[R3]"],
    "[Ri+]": ["[R1+]", "[R3+]"],
    "[Rw+#d16]": ["[R5+#0]", "[R15+#1234H]", "[R2+#-2]"],
    "reg": ["PSW", "DPP3", "MDH"],
    "regb": ["PSW", "MDL"],
    "mem": ["0FA00H", "1234H", "PSW"],
    "bitoff": ["PSW", "R5", "0FD10H", "0FF10H"],
    "bitaddr": ["PSW.3", "R2.15", "0FD00H.0", "0FDFFH.7", "C"],
    "#d3": ["#0", "#7"],
    "#d4": ["#0", "#15"],
    "#d8": ["#0", "#7FH", "#0FFH"],
    "#d16": ["#0", "#1234H", "#0FFFFH"],
    "#m8": ["#55H"],
    "#trap7": ["#0", "#7FH"],
    "#irang2": ["#1", "#4"],
    "#pag10": ["#0", "#3FFH"],
    "#seg8": ["#0", "#0FFH"],
    "cc": ["cc_UC", "cc_Z", "cc_NZ", "cc_V", "cc_NV", "cc_N", "cc_NN", "cc_C", "cc_NC", "cc_EQ", "cc_NE",
           "cc_ULT", "cc_ULE", "cc_UGE", "cc_UGT", "cc_SLE", "cc_SGE", "cc_SGT", "cc_NET", "cc_SLT"],
    # Keil leaves '$'-relative branch targets to its linker, so use labels.
    "rel": ["@BACK", "@FWD"],
    "caddr": ["0C000H", "1234H"],
    "seg": ["0", "3"],
}

# Keep the sweep short for classes that combine with many others.
LIMIT = 24


def main():
    isa = json.load(open(sys.argv[1]))
    cpu = sys.argv[2] if len(sys.argv) > 2 else "c167"
    out = []
    if cpu == "c167":
        out.append("$MOD167")
    out += ["SWEEP SECTION CODE AT 0C000H", "SWP PROC NEAR"]
    seen = set()
    for f in isa["forms"]:
        classes = [o["class"] for o in f["operands"]]
        combos = list(itertools.product(*(SAMPLES[c] for c in classes)))
        if len(combos) > LIMIT:
            step = len(combos) / LIMIT
            combos = [combos[int(i * step)] for i in range(LIMIT)]
        for ops in combos:
            line = f"        {f['mnemonic']:7}{', '.join(ops)}".rstrip()
            if line in seen:
                continue
            seen.add(line)
            label = f"L{len(out)}"
            if "@BACK" in line:
                out.append(f"{label}:")
            out.append(line.replace("@BACK", label).replace("@FWD", label))
            if "@FWD" in line:
                out.append(f"{label}:")
    out += ["SWP ENDP", "SWEEP ENDS", "        END"]
    print("\n".join(out))


if __name__ == "__main__":
    main()
