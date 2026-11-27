#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compiles the C runtime (src/*.c) to A166 library modules (lib/*.a66).

It also compiles the floating-point helpers of LLVM's compiler-rt (soft
float and soft double: __addsf3, __muldf3, __fixsfsi, ...) from the
openc166-llvm fork, under their own licence (Apache-2.0 WITH
LLVM-exception); each such module says so at its top.

The generated modules are committed, so that using the compiler does not
need this step. Run it after changing src/ or the compiler:

    OPENC166_CLANG=/path/to/openc166-llvm/build/bin/clang runtime/build.py

The runtime must not call itself: it is built freestanding, without
builtins, and every module is checked for calls to the names it defines.
"""

import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = ["--target=c166", "-Os", "-ffreestanding", "-fno-builtin", "-S"]
# One runtime per memory model: lib/ for small, <model>/lib/ for the
# others. Calls and returns (CALLS/RETS against CALL/RET) and pointers (16
# or 32 bits) differ, so they do not mix. lib/icall.a66, for calls through
# far function pointers, is hand-written; the large model gets a copy.
MODELS = [
    ("small", "lib"),
    ("tiny", os.path.join("tiny", "lib")),
    ("medium", os.path.join("medium", "lib")),
    ("large", os.path.join("large", "lib")),
]
FAR_CODE = {"small", "large"}

# compiler-rt/lib/builtins: what LLVM calls for float and double.
COMPILER_RT = [
    "addsf3", "subsf3", "mulsf3", "divsf3", "negsf2", "comparesf2",
    "fixsfsi", "fixunssfsi", "fixsfdi", "fixunssfdi",
    "floatsisf", "floatunsisf", "floatdisf", "floatundisf",
    "adddf3", "subdf3", "muldf3", "divdf3", "negdf2", "comparedf2",
    "fixdfsi", "fixunsdfsi", "fixdfdi", "fixunsdfdi",
    "floatsidf", "floatunsidf", "floatdidf", "floatundidf",
    "extendsfdf2", "truncdfsf2",
    "fp_mode",  # rounding mode and inexact flag, which the above ask for
]
CRT_NOTICE = """; Compiled from LLVM compiler-rt, lib/builtins/{name}.c, by openc166's
; runtime/build.py. Part of the LLVM Project, under the Apache License v2.0
; with LLVM Exceptions. See https://llvm.org/LICENSE.txt for license
; information. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""


def check(out):
    with open(out) as f:
        text = f.read()
    defined = set(re.findall(r"^\s*PUBLIC\s+(\S+)", text, re.M))
    called = set(re.findall(r"^\s*CALL\s+(\S+)", text, re.M))
    called |= set(re.findall(r"^\s*CALLS\s+SEG\s+(\S+),", text, re.M))
    if defined & called:
        sys.exit(f"{out}: calls itself: {sorted(defined & called)}")


def main():
    fork = os.environ.get("OPENC166_LLVM") or os.path.join(
        os.path.dirname(os.path.dirname(HERE)), "openc166-llvm"
    )
    clang = os.environ.get("OPENC166_CLANG") or os.path.join(fork, "build", "bin", "clang")
    for model, libdir in MODELS:
        build(clang, fork, model, os.path.join(HERE, libdir))


def build(clang, fork, model, libdir):
    flags = FLAGS + ["-mabi=" + model]
    os.makedirs(libdir, exist_ok=True)
    if model in FAR_CODE and libdir != os.path.join(HERE, "lib"):
        shutil.copyfile(os.path.join(HERE, "lib", "icall.a66"),
                        os.path.join(libdir, "icall.a66"))
    for src in sorted(glob.glob(os.path.join(HERE, "src", "*.c"))):
        name = os.path.splitext(os.path.basename(src))[0]
        out = os.path.join(libdir, name + ".a66")
        subprocess.run(
            [clang, *flags, os.path.join("src", name + ".c"), "-o", out],
            cwd=HERE,
            check=True,
        )
        check(out)
        print(out)

    builtins = os.path.join(fork, "compiler-rt", "lib", "builtins")
    for name in COMPILER_RT:
        out = os.path.join(libdir, name + ".a66")
        asm = subprocess.run(
            [clang, *flags, "-I", builtins, name + ".c", "-o", "-"],
            cwd=builtins,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        with open(out, "w") as f:
            f.write(CRT_NOTICE.format(name=name) + asm)
        check(out)
        print(out)


if __name__ == "__main__":
    main()
