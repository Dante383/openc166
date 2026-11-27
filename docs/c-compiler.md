# The C compiler

openc166 has a C compiler: Clang with a C166 back end, in the
`openc166-llvm` fork of llvm-project (branch `c166`). It compiles C99 with
the TASKING C166/ST10 Classic extensions to A166 assembly, which openc166
assembles and links with the C runtime in [runtime/](../runtime/README.md).
The design and the reasons behind it are in
[c-compiler-design.md](c-compiler-design.md).

Status: TASKING's small and tiny memory models, integers up to 64 bits,
`float` and `double`, TASKING keywords and
intrinsics for payloads and drivers. Checked by running compiled C in the
simulator (`python/tests/test_c_compiler.py`), including a MiniMon-style
payload on the simulated 5WY19.

## Building and running

Build the fork (only the C166 target; see its `C166.md`), then:

```sh
clang --target=c166 -Os main.c util.c -o prog.hex \
  -Wl,--class,CPROGRAM=0F200h-0F8FFh,--class,CNEARROM=0F200h-0F8FFh \
  -Wl,--class,CNEAR=0F900h-0FBBFh
```

- `-Os` is the optimisation to use for ROM: `-O2` trades much more size
  for speed (it turns a bitwise CRC into a 512-byte table, and unrolls).
- `clang -c` writes an A166 module (`main.o` is assembly text); linking runs
  openc166 on the modules, which must be on `PATH` or next to `clang`.
- The startup code (`runtime/cstart.a66`) is linked first and the runtime
  helpers as library modules. `-nostartfiles`, `-nodefaultlibs` and
  `-nostdlib` leave them out. Clang finds the runtime in openc166's source
  tree, in `share/openc166/runtime` next to an installed `openc166`, or
  where `OPENC166_RUNTIME` points.
- `-Wl,` passes options to openc166: `--class` for the memory map,
  `--section`, `-l` for a listing, `-g` for debug information.
- `double` is 64 bits; `-mdouble=32` (TASKING's `-F`) makes it the same
  as `float`, smaller and faster. Modules built with different settings do
  not mix. Floating point is in software (LLVM compiler-rt).
- `-g` puts C line markers into the assembly, so openc166's debug
  information and ELF output, and so the debugger and Ghidra, show C lines.

The entry point is `__cstart`. It sets up the user stack, initialises the
variables, calls `main` and returns `main`'s result in R4, keeping the
caller's R0 and R6-R9: a loader or existing firmware can call a C program
and get control back (with `CALLS` in the small model, `CALLA` in the
tiny one). A program that starts from reset must first set up
SYSCON, BUSCON, the system stack and the watchdog itself.

## Memory models

`-mabi=` picks TASKING's memory model:

| `-mabi=` | Code | Data | Function pointers |
|---|---|---|---|
| `small` (default) | far: anywhere in 16 MB, `CALLS`/`RETS` | near, first 64 KB (DPPn = n) | 32 bits |
| `tiny` | near: first 64 KB, `CALL`/`RET` | near, first 64 KB | 16 bits |

In the small model every function is far, as in TASKING, except:

- a `_near` function (`int _near f(int);`), called with `CALLA`/`CALLR`
  and returning with `RET`: it must be in the same segment as its
  callers. Use it to call TASKING `_near` functions;
- a `static` function whose address is never taken, which is in the same
  code section as all its callers, so it is near without asking.

A call through a function pointer goes through `__c166_icall` in the
runtime (`PUSH R5`, `PUSH R4`, `RETS`, as the stock 5WY19 firmware does at
2000H). The output is `$SEGMENTED`, so openc166 places code above 64 KB;
near data references are written as `SOF name` (their address in the first
64 KB) and SFRs through `ASSUME DPP3:SYSTEM`. Each model has its own
runtime (`runtime/` and `runtime/tiny/`), which the driver picks; modules
of different models do not mix. Medium, large and huge need far data by
default and are not done yet.

## Memory map

| Class | What |
|---|---|
| `CPROGRAM` | code |
| `CNEARROM` | constants, and the initial values of variables (`C166_DATA_INIT`) |
| `CNEAR` | variables (`C166_DATA`, `C166_BSS`) and the 512-byte user stack |
| `CBITWORDS` | `_bitword` variables, placed in the bit-addressable RAM |

openc166 places sections in the first free space unless told otherwise, so
give the classes with `--class`. In the small model all data is near: it
must lie in the pages the DPPs select (by default DPPn = n, the first
64 KB).

## Calling convention

TASKING Classic's, so C can call and be called by TASKING-built code,
including the stock 5WY19 firmware:

- arguments in R12, R13, R14, R15, a 32-bit one in two registers (low word
  first); one that no longer fits, a float, a struct by value and every
  argument after them go on the user stack, and so do the `...` of a
  variadic function;
- results in R4 (`char` in RL4), 32 bits in R4:R5;
- R6-R9 saved by the called function; R0 is the user stack pointer; the
  system stack holds only return addresses;
- C names get a leading underscore (`main` is `_main`).

Stack arguments are laid out first argument lowest, starting at R0 on
entry; that order is still to be checked against a Classic compiler.

## TASKING extensions

The keywords need no header. Intrinsics are in `<c166.h>`, the registers
of the C167CR/CS in `<reg167.h>` (or `<regc167cs.h>`), generated from
openc166's `REG167.INC` by `tools/reg_h.py`.

| Extension | |
|---|---|
| `_sfr`, `_esfr`, `_xsfr` | a register, used by its name (`MOV DP2, R4`) |
| `_sfrbit`, `_esfrbit` | an SFR bit by name: `BSET`, `BCLR`, `BMOV` |
| `_atbit(name, n)` | bit `n` of an SFR or `_bitword` (`_sfrbit P2_3 _atbit(P2, 3);`) |
| `_bit` | a `_Bool`; as a variable, a byte, not a bit in the bit-addressable RAM |
| `_bitword` | a word in the bit-addressable RAM |
| `_at(address)` | a variable or constant at a fixed address (variables without initial value) |
| `_interrupt(n)` | an interrupt function with a vector at 4 × n; `-1` gives no vector |
| `_using(...)` | accepted; interrupt functions push what they use (`_using(push)`) |
| `_inline`, `_noinline` | inlining |
| `_near`, `_system`, `_iram`, `_bita` | accepted (data is near) |
| `_far`, `_huge`, `_shuge` | pointers (see below); variables not yet |
| `_xnear` | an error |

`_far` and `_huge` pointers are 32 bits, low word first: a `_huge`
(and `_shuge`) pointer is segment offset and segment, read and written
with `EXTS Rseg, #1`; a `_far` pointer is page offset and page number,
with `EXTP Rpage, #1`. Arithmetic on them is 32-bit. A near pointer
converts to either, implicitly, through the DPP that selects its page,
and `_far` and `_huge` convert into each other; making one near is an
error. `_mkhp(sof, seg)` and `_mkfp(pof, pag)` build one, `_sof`, `_seg`,
`_pof` and `_pag` take one apart. So code in the small model can read
and write all 16 MB, for example flash above 64 KB:

```c
const unsigned char _huge *flash = _mkhp(0x0000, 0x08); /* 80000H */
```

An interrupt function saves the registers it uses on the user stack it
interrupted, and MDC/MDH/MDL when it multiplies, divides, calls a function
or has inline assembly, and returns with `RETI`.

Intrinsics: `_nop`, `_srvwdt`, `_diswdt`, `_einit`, `_idle`, `_pwrdn`,
`_int166`, `_atomic`, `_rol`, `_ror`, `_prior`, `_mul32`, `_mulu32`,
`_div32`, `_divu32`, `_mod32`, `_modu32`, `_getbit`, `_putbit`, `_bfld`,
`_testset`, `_testclear`. The bit intrinsics take an SFR or a `_bitword`
(`_testset`/`_testclear`: an `_sfrbit` or `_atbit` bit) and use the bit
instructions, so they are atomic.

Inline assembly: `__asm__("...")` with `%0` operands; `"r"` gives a
register, `"i"(&var)` the A166 name of a variable or SFR.

## Not yet

- `_far`/`_huge` variables (their sections and initial values), and the
  medium, large and huge memory models.
- `_near` function pointers (all function pointers are far in the small
  model), and register bank switching.
- Structs passed by value (an error). Structs returned by value work, but
  through a hidden pointer argument, not TASKING's block on the user stack,
  so such functions cannot be called from TASKING-built code.
- Register bank switching for interrupts (`_using(name)` falls back to
  pushing registers), `#pragma` (`noframe`, `regdef`, ...), `_packed`,
  `_noalign`, `_stackparm`.
- `_bit` variables as bits in the bit-addressable RAM.
- Alignment above 2 bytes (`_Alignas(4)`): A166 has only `EVEN`.
- A C library beyond `memcpy`, `memmove` and `memset`.
