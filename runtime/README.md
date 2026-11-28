# C runtime and library for the C166 compiler

What a C program compiled with the C166 clang (the `openc166-llvm` fork)
links with. The clang driver finds this directory by itself: in openc166's
source tree, installed as `share/openc166/runtime` next to `bin/openc166`,
or wherever `OPENC166_RUNTIME` points. It also puts `include/` on the
include path.

- `cstart.a66`: startup code, linked first unless `-nostartfiles`.
  `__cstart` sets the user stack (R0, in `C166_USTACK`), copies
  initialised variables from ROM (`C166_DATA_INIT`) to RAM (`C166_DATA`),
  clears `C166_BSS` (and does the same for `_far`/`_huge` data), calls
  `main` and returns its result in R4. `exit` and `_exit` return from
  `__cstart` too, with their status. It keeps the caller's R0 and R6-R9,
  so a loader or existing firmware can call a C program. It does not set
  up SYSCON, BUSCON, the DPPs, the system stack or the watchdog.
- `lib/*.a66`: library modules: the helpers the compiler calls (32- and
  64-bit division and remainder, 64-bit multiplication, 32-bit shifts by a
  variable amount, soft float and double) and the C library. openc166
  links one only when the program needs a name it defines (`--lib`), so
  they cost nothing otherwise, and a program that defines a name itself
  (`_write`, say) gets its own. Not linked with `-nodefaultlibs` or
  `-nostdlib`.
- One runtime per memory model, which the driver picks by `-mabi=`: `lib/`
  for small, `tiny/lib/`, `medium/lib/`, `large/lib/` and `huge/lib/` for
  the others (calls, returns and pointer sizes differ, so they do not
  mix). `tiny/` and `medium/` have their own `cstart.a66` (near
  `__cstart`, `CALL main`), and so does `huge/` (variables over 64 KB);
  large uses `cstart.a66`, as small does.
- `src/*.c`: the compiler helpers' source; `libc/`: the C library's;
  `include/`: its headers. `build.py` compiles them into each model's
  `lib/` with the C166 clang. The results are committed, so using the
  compiler does not need that step. `lib/icall.a66` (calls through far
  function pointers) and `libc/setjmp.a66` are hand-written; `build.py`
  copies the first to `large/lib/` and `huge/lib/` and puts each model's
  controls in front of the second.

## The C library

C99's freestanding headers come with clang (`stdint.h`, `stddef.h`,
`stdarg.h`, `limits.h`, `float.h`, `stdbool.h`, ...). `include/` adds:

| Header | What |
|---|---|
| `string.h` | `mem*`, `str*` (all of C99's), `strtok`, `strerror` |
| `stdlib.h` | `abs`/`div` families, `atoi`, `strtol`/`strtoul`/`strtoll`/`strtoull`, `strtod`/`atof`, `rand`, `malloc`/`calloc`/`realloc`/`free`, `qsort`, `bsearch`, `exit`, `atexit`, `abort`, `getenv` (no environment: `NULL`) |
| `stdio.h` | `printf`, `fprintf`, `sprintf`, `snprintf` and the `v` forms; `putchar`, `puts`, `fputs`, `fputc`, `fwrite`; `getchar`, `fgetc`, `fgets`, `fread`, `ungetc`; `perror` |
| `ctype.h` | the `is*` functions, `tolower`, `toupper` (ASCII) |
| `math.h` | C99's functions in double and float: trigonometry, `exp`/`log` families, `pow`, `sqrt`, `cbrt`, `hypot`, rounding, `fmod`, `frexp`/`ldexp`/`modf`, ... |
| `setjmp.h`, `errno.h`, `assert.h` | as in C |

- **Input and output** go through `_read(fd, buffer, size)` and
  `_write(fd, buffer, count)`, TASKING's names; stdin, stdout and stderr
  are file descriptors 0, 1 and 2, and there are no files. The library's
  own `_read` and `_write` use ASC0, polled: the program sets up the baud
  rate and pins first. A program that defines `_write` (say, into a
  buffer, or to the CAN bus) gets its own instead. Output is not buffered
  beyond one call.
- **printf** formats integers (to `long long`), characters, strings and
  pointers, with flags, width, precision and the length modifiers.
  Floating-point conversions are not supported yet: they take their
  argument and print nothing. There is no `scanf`.
- **malloc** takes its memory from `__c166_heap`, 1024 bytes by default (in
  `CNEAR`, or `CHUGE` with far data). A program that defines both
  `char __c166_heap[n]` and `const size_t __c166_heap_size = n` uses its
  own.
- **The user stack** is 512 bytes. `_ustack(n);` (from `<c166.h>`, at file
  scope) adds n bytes. The math functions need it: up to about 1.8 KB for
  the double ones (`atan2`, `sin`), about 1 KB for the float ones, measured
  in the simulator.
- **Sizes.** The math functions are LLVM libc's, correctly rounded but for
  the last pass (within about an ulp), built with small tables. With soft
  float they are large: `sin` and its helpers take about 30 KB of code,
  `exp` 14 KB, `sqrt` 10 KB. The float versions are not much smaller,
  since they compute in double inside.
- With `-mdouble=32` (`double` is `float`), `<math.h>` binds each double
  function to its float one: `sin` calls `sinf`.

## Licences

The floating-point helpers (`addsf3.a66`, `muldf3.a66`, ...) are LLVM's
compiler-rt builtins, and the math functions (`sin.a66`, `powf.a66`, ...)
LLVM libc's, compiled for the C166 by `build.py` from the openc166-llvm
fork (`OPENC166_LLVM` names it). They are under the Apache License v2.0
with LLVM Exceptions (https://llvm.org/LICENSE.txt), as the comment at the
top of each says. The rest, the C library in `libc/` and `include/`
included, is openc166's (MIT).

## Memory classes

The compiler puts code in class `CPROGRAM`, constants and initial values in
`CNEARROM`, and variables and the user stack in `CNEAR`; `_huge` and `_far`
data, and in the medium and large models all unqualified data, in `CHUGE`,
`CHUGEROM`, `CFAR` and `CFARROM`. openc166 places them in the first free
space unless told where, so a program gives the memory map with
`--class`, for example for code in flash and variables in the C167's
internal RAM:

```sh
clang --target=c166 -Os main.c -o main.hex \
  -Wl,--class,CPROGRAM=10000h-1FFFFh,--class,CNEARROM=8000h-0BFFFh \
  -Wl,--class,CNEAR=0F600h-0FBFFh
```

Near addresses go through the DPPs, so `CNEAR` and `CNEARROM` must lie in
the pages the DPPs select (by default DPPn = n, the first 64 KB).
