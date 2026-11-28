#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compiles the C runtime (src/*.c: helpers the compiler calls) and the C
library (libc/*.c, with the headers in include/) to A166 library modules
(lib/*.a66, and <model>/lib/*.a66 for each memory model but small).
libc/setjmp.a66 is hand-written and gets the model's controls in front.

It also compiles the floating-point helpers of LLVM's compiler-rt (soft
float and soft double: __addsf3, __muldf3, __fixsfsi, ...) from the
openc166-llvm fork, under their own licence (Apache-2.0 WITH
LLVM-exception); each such module says so at its top.

The generated modules are committed, so that using the compiler does not
need this step. Run it after changing src/ or the compiler:

    OPENC166_CLANG=/path/to/openc166-llvm/build/bin/clang runtime/build.py

The runtime must not call itself: it is built freestanding, without
builtins, and every module in src/ is checked for calls to the names it
defines. (Library modules may: getc calls fgetc.)
"""

import glob
import os
import re
import shutil
import tempfile
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
FAR_DATA = {"medium", "large"}

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
# LLVM libc's math functions (libc/shared/math: header-only C++), each
# wrapped as a C function in a module of its own: name -> (result,
# parameters) for the double version; the float one is name + "f". cosh,
# sinh and tanh exist for float only there; libc/ has the double ones.
MATH1 = ["acos", "asin", "atan", "cbrt", "ceil", "cos", "exp", "exp2",
         "expm1", "fabs", "floor", "log", "log10", "log1p", "log2",
         "nearbyint", "rint", "round", "sin", "sqrt", "tan", "trunc"]
MATH2 = ["atan2", "copysign", "fmax", "fmin", "fmod", "hypot", "pow",
         "remainder"]
MATH = {name: ("T", ["T x"]) for name in MATH1}
MATH.update({name: ("T", ["T x", "T y"]) for name in MATH2})
MATH.update({
    "frexp": ("T", ["T x", "int *e"]),
    "ldexp": ("T", ["T x", "int e"]),
    "scalbn": ("T", ["T x", "int e"]),
    "modf": ("T", ["T x", "T *ip"]),
    "lround": ("long", ["T x"]),
})
FLOAT_ONLY = ["cosh", "sinh", "tanh"]
# Optimised for ROM: no correctly rounded second pass (results stay within
# an ulp or so), small tables. LLVM libc knows no fenv_t for the C166 and
# has no floating-point environment to query here.
MATH_FLAGS = ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-DLIBC_FULL_BUILD",
              "-DLIBC_NAMESPACE=__llvm_libc_c166",
              # errno is the C library's (errno.h)
              "-DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_SYSTEM_INLINE",
              "-DLIBC_MATH=(LIBC_MATH_SKIP_ACCURATE_PASS|LIBC_MATH_SMALL_TABLES)"]
MATH_PRELUDE = """#define LLVM_LIBC_TYPES_FENV_T_H
typedef unsigned int fenv_t;
"""
MATH_NOTICE = """; Compiled from LLVM libc, libc/shared/math/{name}.h, by openc166's
; runtime/build.py. Part of the LLVM Project, under the Apache License v2.0
; with LLVM Exceptions. See https://llvm.org/LICENSE.txt for license
; information. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""


def math_wrapper(name, result, params):
    args = ", ".join(p.split()[-1].lstrip("*") for p in params)
    return (MATH_PRELUDE + f'#include "shared/math/{name}.h"\n'
            f'extern "C" {result} {name}({", ".join(params)}) {{\n'
            f"  return LIBC_NAMESPACE::shared::{name}({args});\n}}\n")


CRT_NOTICE = """; Compiled from LLVM compiler-rt, lib/builtins/{name}.c, by openc166's
; runtime/build.py. Part of the LLVM Project, under the Apache License v2.0
; with LLVM Exceptions. See https://llvm.org/LICENSE.txt for license
; information. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""


def strip(asm):
    """The module without LLVM's comments (block names, spill notes, values
    in hex): a third of the text, of no use to a reader of the library.
    Comments that start a line are kept."""
    out = []
    for line in asm.split("\n"):
        if line.lstrip().startswith(";") and line[:1] in (" ", "\t"):
            continue
        code = line.split(";", 1)[0].rstrip()
        out.append(code if code else line)
    return "\n".join(out)


def tidy(out, notice=""):
    with open(out) as f:
        text = f.read()
    with open(out, "w") as f:
        f.write(notice + strip(text))


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
    for src in sorted(glob.glob(os.path.join(HERE, "libc", "*.c"))):
        name = os.path.splitext(os.path.basename(src))[0]
        out = os.path.join(libdir, name + ".a66")
        subprocess.run(
            [clang, *flags, "-nostdlibinc", "-isystem", "include",
             os.path.join("libc", name + ".c"), "-o", out],
            cwd=HERE,
            check=True,
        )
        tidy(out)
        print(out)
    with open(os.path.join(HERE, "libc", "setjmp.a66")) as f:
        body = f.read()
    controls = "$NOMACRO\n$CASE\n$MOD167\n"
    if model != "tiny":
        controls += "$SEGMENTED\n"
    for flag, models in (("FARCODE", FAR_CODE), ("FARDATA", FAR_DATA)):
        controls += f"$SET ({flag})\n" if model in models else f"$RESET ({flag})\n"
    controls += "$INCLUDE(REG167.INC)\n"
    with open(os.path.join(libdir, "setjmp.a66"), "w") as f:
        f.write(controls + body)

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
        tidy(out)
        check(out)
        print(out)

    libc = os.path.join(fork, "libc")
    wrappers = []
    for name, (result, params) in MATH.items():
        for t, suffix in (("double", ""), ("float", "f")):
            wrappers.append((name + suffix, name + suffix, result.replace("T", t),
                             [p.replace("T", t, 1) for p in params]))
    for name in FLOAT_ONLY:
        wrappers.append((name + "f", name + "f", "float", ["float x"]))
    with tempfile.TemporaryDirectory() as tmp:
        for module, name, result, params in wrappers:
            out = os.path.join(libdir, module + ".a66")
            # A file of the module's name, which names its sections.
            src = os.path.join(tmp, module + ".cpp")
            with open(src, "w") as f:
                f.write(math_wrapper(name, result, params))
            subprocess.run(
                [clang, *flags, *MATH_FLAGS, "-nostdlibinc", "-isystem",
                 os.path.join(HERE, "include"), "-I", libc, src, "-o", out],
                cwd=tmp,
                check=True,
            )
            tidy(out, MATH_NOTICE.format(name=name))
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
            f.write(CRT_NOTICE.format(name=name) + strip(asm))
        check(out)
        print(out)


if __name__ == "__main__":
    main()
