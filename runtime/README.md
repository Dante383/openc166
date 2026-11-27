# C runtime for the C166 compiler

What a C program compiled with the C166 clang (the `openc166-llvm` fork)
links with. The clang driver finds this directory by itself: in openc166's
source tree, installed as `share/openc166/runtime` next to `bin/openc166`,
or wherever `OPENC166_RUNTIME` points.

- `cstart.a66`: startup code, linked first unless `-nostartfiles`.
  `__cstart` sets the user stack (R0, 512 bytes in `C166_USTACK`), copies
  initialised variables from ROM (`C166_DATA_INIT`) to RAM (`C166_DATA`),
  clears `C166_BSS`, calls `main` and returns its result in R4. It keeps
  the caller's R0 and R6-R9, so a loader or existing firmware can call a
  C program. It does not set up SYSCON, BUSCON, the DPPs, the system stack
  or the watchdog.
- `lib/*.a66`: helpers the compiler calls (32- and 64-bit division and
  remainder, 64-bit multiplication, 32-bit shifts by a variable amount,
  `memcpy`, `memmove`, `memset`, and soft float and double).
  Library modules: openc166 links one only when the program needs a name
  it defines (`--lib`), so they cost nothing otherwise. Not linked with
  `-nodefaultlibs` or `-nostdlib`.
- `src/*.c`: the helpers' source. `build.py` compiles them to `lib/` with
  the C166 clang; the results are committed, so using the compiler does not
  need that step.
- One runtime per memory model, which the driver picks by `-mabi=`: `lib/`
  for small, `tiny/lib/`, `medium/lib/` and `large/lib/` for the others
  (calls, returns and pointer sizes differ, so they do not mix). `tiny/`
  and `medium/` have their own `cstart.a66` (near `__cstart`, `CALL main`);
  large uses `cstart.a66`, as small does. `lib/icall.a66` (calls through
  far function pointers) is hand-written; `build.py` copies it to
  `large/lib/`.
- The floating-point helpers (`addsf3.a66`, `muldf3.a66`, `comparesf2.a66`,
  ...) are LLVM's compiler-rt builtins compiled for the C166 by `build.py`
  from the openc166-llvm fork (`OPENC166_LLVM` names it). They are under
  the Apache License v2.0 with LLVM Exceptions
  (https://llvm.org/LICENSE.txt), as the comment at the top of each says;
  the rest of openc166 is MIT.

## Memory classes

The compiler puts code in class `CPROGRAM`, constants and initial values in
`CNEARROM`, and variables and the user stack in `CNEAR`. openc166 places
them in the first free space unless told where, so a program gives the
memory map with `--class`, for example for code in flash and variables in
the C167's internal RAM:

```sh
clang --target=c166 -O2 main.c -o main.hex \
  -Wl,--class,CPROGRAM=10000h-1FFFFh,--class,CNEARROM=10000h-1FFFFh \
  -Wl,--class,CNEAR=0F600h-0FBFFh
```

Small memory model: near addresses go through the DPPs, so `CNEAR` and
`CNEARROM` must lie in the pages the DPPs select (by default DPPn = n, the
first 64 KB).
