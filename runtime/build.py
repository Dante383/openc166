#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compiles the C runtime (src/*.c) to A166 library modules (lib/*.a66).

The generated modules are committed, so that using the compiler does not
need this step. Run it after changing src/ or the compiler:

    OPENC166_CLANG=/path/to/openc166-llvm/build/bin/clang runtime/build.py

The runtime must not call itself: it is built freestanding, without
builtins, and every module is checked for calls to the names it defines.
"""

import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = ["--target=c166", "-Os", "-ffreestanding", "-fno-builtin", "-S"]


def main():
    clang = os.environ.get("OPENC166_CLANG") or os.path.join(
        os.path.dirname(os.path.dirname(HERE)), "openc166-llvm", "build", "bin", "clang"
    )
    os.makedirs(os.path.join(HERE, "lib"), exist_ok=True)
    for src in sorted(glob.glob(os.path.join(HERE, "src", "*.c"))):
        name = os.path.splitext(os.path.basename(src))[0]
        out = os.path.join(HERE, "lib", name + ".a66")
        subprocess.run(
            [clang, *FLAGS, os.path.join("src", name + ".c"), "-o", out],
            cwd=HERE,
            check=True,
        )
        with open(out) as f:
            text = f.read()
        defined = set(re.findall(r"^\s*PUBLIC\s+(\S+)", text, re.M))
        called = set(re.findall(r"^\s*CALL\s+(\S+)", text, re.M))
        if defined & called:
            sys.exit(f"{out}: calls itself: {sorted(defined & called)}")
        print(out)


if __name__ == "__main__":
    main()
