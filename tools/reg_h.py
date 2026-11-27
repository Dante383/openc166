#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes a C register header for the C166 compiler from a register file.

    tools/reg_h.py inc/REG167.INC > reg167.h

Each register (DEFR) becomes an _sfr declaration, or _esfr for the
extended SFR area (F000H-F1FFH), and each named bit (BIT) an _sfrbit.
These are TASKING C166 declarations: they carry no addresses, because the
compiler writes the names into its assembly and openc166 takes the
addresses from the same register file, which the compiler's output
includes. So the header and the register file cannot disagree.
"""

import os
import re
import sys


def main():
    path = sys.argv[1]
    name = os.path.splitext(os.path.basename(path))[0].lower()
    guard = "__" + name.upper() + "_H"
    regs, bits = [], []
    with open(path) as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            m = re.match(r"(\w+)\s+DEFR\s+([0-9A-Fa-f]+)H", line)
            if m:
                address = int(m.group(2), 16)
                regs.append((m.group(1), address))
                continue
            m = re.match(r"(\w+)\s+BIT\s+(\w+)\.(\d+)", line)
            if m:
                bits.append((m.group(1), m.group(2), int(m.group(3))))
    out = sys.stdout
    out.write(f"""/* {name}.h: special function registers and bits for the C166 compiler.
 *
 * Generated from openc166's {os.path.basename(path)} by openc166's tools/reg_h.py;
 * do not edit. The declarations have no addresses: the compiler writes the
 * names into its assembly, and openc166 takes the addresses from
 * {os.path.basename(path)}, which every module the compiler writes includes.
 */

#ifndef {guard}
#define {guard}

#ifndef __c166__
#error "{name}.h is for the C166 target (--target=c166)"
#endif

""")
    for reg, address in regs:
        kind = "_esfr" if 0xF000 <= address < 0xF200 else "_sfr"
        out.write(f"{kind} {reg}; /* {address:04X}H */\n")
    out.write("\n")
    for bit, reg, n in bits:
        out.write(f"_sfrbit {bit}; /* {reg}.{n} */\n")
    out.write(f"\n#endif /* {guard} */\n")


if __name__ == "__main__":
    main()
