# The C compiler

openc166 has a C compiler: Clang with a C166 back end, in the
[openc166-llvm](https://github.com/Dante383/openc166-llvm) fork of
llvm-project (branch `c166`). It compiles C99 with
the TASKING C166/ST10 Classic extensions to A166 assembly, which openc166
assembles and links with the C runtime in [runtime/](../runtime/README.md).
The design and the reasons behind it are in
[c-compiler-design.md](c-compiler-design.md).

Status: TASKING's tiny, small, medium, large and huge memory models, integers up
to 64 bits, `float` and `double`, structs by value, TASKING keywords and
intrinsics for payloads and drivers, and a C library (`stdio.h`,
`string.h`, `stdlib.h`, `math.h`, ...). Checked by running compiled C in the
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
- The startup code (`runtime/cstart.a66`) is linked first, and the runtime
  helpers and the C library as library modules. `-nostartfiles`,
  `-nodefaultlibs` and `-nostdlib` leave them out, `-nostdlibinc` the C
  library's headers. Clang finds the runtime in openc166's source tree, in
  `share/openc166/runtime` next to an installed `openc166`, or where
  `OPENC166_RUNTIME` points.
- `-Wl,` passes options to openc166: `--class` for the memory map,
  `--section`, `-l` for a listing, `-g` for debug information.
- `double` is 64 bits; `-mdouble=32` (TASKING's `-F`) makes it the same
  as `float`, smaller and faster. Modules built with different settings do
  not mix. Floating point is in software (LLVM compiler-rt).
- `-g` puts C line markers into the assembly, so openc166's debug
  information and ELF output, and so the debugger and Ghidra, show C lines.

The entry point is `__cstart`. It sets up the user stack, initialises the
variables, calls `main` and returns `main`'s result in R4 (or the status
given to `exit`), keeping the caller's R0 and R6-R9: a loader or existing
firmware can call a C program and get control back (with `CALLS` in the
small and large models, `CALLA` in the tiny and medium ones). A program
that starts from reset must first set up SYSCON, BUSCON, the system stack
and the watchdog itself.

## C library

openc166's own (MIT) in `runtime/libc` with its headers in
`runtime/include`, and LLVM libc's math functions; one build per memory
model. [runtime/README.md](../runtime/README.md) lists what is there.
In short:

- `printf` and the other output go through `_write(fd, buffer, count)`,
  input through `_read`, as in TASKING. The library's own use ASC0
  (polled; set up the baud rate first); define `_write` to send output
  elsewhere. `printf` has no floating-point conversions yet, and there is
  no `scanf`.
- The user stack is 512 bytes; `_ustack(n);` from `<c166.h>` adds n bytes.
  The math functions need up to about 1.8 KB.
- `malloc` uses a 1024-byte heap unless the program defines
  `__c166_heap` and `__c166_heap_size` itself.
- The math functions are large in soft float (`sin` with what it needs:
  about 30 KB of code). With `-mdouble=32`, `sin` is `sinf`.

## Memory models

`-mabi=` picks TASKING's memory model:

| `-mabi=` | Code | Data | Function pointers |
|---|---|---|---|
| `small` (default) | far: anywhere in 16 MB, `CALLS`/`RETS` | near, first 64 KB (DPPn = n) | 32 bits |
| `tiny` | near: first 64 KB, `CALL`/`RET` | near, first 64 KB | 16 bits |
| `medium` | near: one segment, `CALL`/`RET` | far: anywhere in 16 MB | 16 bits |
| `large` | far, as in small | far, as in medium | 32 bits |
| `huge` | far, as in small | far, objects over 64 KB | 32 bits |

In the small model every function is far, as in TASKING, except:

- a `_near` function (`int _near f(int);`), called with `CALLA`/`CALLR`
  and returning with `RET`: it must be in the same segment as its
  callers. Use it to call TASKING `_near` functions;
- a `static` function whose address is never taken, which is in the same
  code section as all its callers, so it is near without asking.

A call through a function pointer goes through `__c166_icall` in the
runtime (`PUSH R5`, `PUSH R4`, `RETS`, as the stock 5WY19 firmware does at
2000H). The output is `$SEGMENTED` in every model but tiny, so openc166
places code and data above 64 KB; near data references are written as
`SOF name` (their address in the first 64 KB) and SFRs through
`ASSUME DPP3:SYSTEM`. Each model has its own runtime (`runtime/lib` for
small, `runtime/<model>/lib` for the others, and `runtime/tiny/` and
`runtime/medium/` have their own `cstart.a66`), which the driver picks;
modules of different models do not mix.

In the medium and large models data is far:

- an unqualified pointer is 32 bits, in the `_huge` layout (segment
  offset, segment), and is accessed with `EXTS`; pointer arithmetic is
  32-bit. `size_t` stays 16 bits and `ptrdiff_t` is `long`, as in TASKING;
- unqualified variables and constants (string literals too) go into the
  `_huge` sections, `CHUGE` and `CHUGEROM` (see below), anywhere in the
  16 MB. TASKING puts them in its `_far` sections; with `_far` pointers in
  the `_huge` layout here, the `_huge` ones fit;
- locals stay on the user stack, which is near. Reading and writing a
  local is near, even through a pointer the compiler can follow; a pointer
  to a local that is passed on is made far through the DPP that maps it
  (the DPPn register at 0FE00H + 2n), so the user stack (`CNEAR`) must lie
  in the pages the DPPs select;
- SFRs, `_sfrbit`, `_bitword`, `_system`, `_iram` and `_bita` data are near
  in every model, and a `_near` variable goes into the near sections.

Medium's functions are near, all in one segment, and its function pointers
are 16 bits (`CALLI`), so code that calls through them has no
`__c166_icall`.

The huge model is the large one with objects of any size, as in TASKING:
`size_t` is 32 bits, unqualified and `_huge` variables and constants go
into `XDATA` and `XCONST` sections (still classes `CHUGE` and
`CHUGEROM`), which may be over 64 KB and cross segment boundaries, and
`__cstart` (`runtime/huge/cstart.a66`) fills and clears them with 32-bit
lengths. The second word of a 32-bit access, and the second byte of an
unaligned one, carries into the segment. In the other models `_huge` and
unqualified far data share `HDATA` sections, at most 64 KB in all.
`_shuge` is `_huge`.

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
  first); one that no longer fits, a float, a struct by value (a copy, in
  words) and every argument after them go on the user stack, and so do the
  `...` of a variadic function;
- results in R4, 32 bits in R4:R5; a `char` only in RL4 (TASKING leaves
  RH4 as it is, so the caller extends it); a `_bit`/`_Bool` in USR0
  (PSW.6);
- a struct or `double` result in a block the caller reserves on the user
  stack above the stack arguments; the called function writes it there and
  returns its address in R4. (A variadic function cannot know where that
  is, so there the block's address is a hidden first argument instead.)
- R6-R9 saved by the called function; R0 is the user stack pointer; the
  system stack holds only return addresses;
- C names get a leading underscore (`main` is `_main`).

C modules use `$CASE`, so their names reach the linker as written. An
assembly module without `$CASE` gives the linker its names in upper case,
as Keil's L166 does: to call its `Sat_Add` from C, declare it under that
name (`int sat_add(int, int) __asm__("SAT_ADD");`), and a C function that
such a module calls needs an upper-case name too (`__asm__("C_FUNC")`).
When a name differs only in case, error OC15 says so.

Stack arguments are laid out first argument lowest, starting at R0 on
entry, as the stock 5WY19 firmware's callees read them (its non-leaf
functions push R15..R12 at entry, putting the register arguments in the
same order just below).

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
| `_using(bank)`, `_using(push)` | an interrupt function's own register bank, or pushed registers (see below) |
| `_inline`, `_noinline` | inlining |
| `_near`, `_system`, `_iram`, `_bita` | accepted (data is near) |
| `_far`, `_huge`, `_shuge` | pointers and variables anywhere in 16 MB (see below) |
| `_xnear` | an error |

`_far` and `_huge` pointers are 32 bits, low word first: segment offset
and segment, a linear address, read and written with `EXTS Rseg, #1`.
Arithmetic on them is 32-bit. A near pointer converts to either,
implicitly, through the DPP that selects its page, and `_far` and `_huge`
convert into each other; making one near is an error. `_mkhp(sof, seg)`
and `_mkfp(pof, pag)` build one, `_sof`, `_seg`, `_pof` and `_pag` take one
apart. So code can read and write all 16 MB, for example flash above
64 KB:

```c
const unsigned char _huge *flash = _mkhp(0x0000, 0x08); /* 80000H */
```

TASKING keeps `_far` pointers as page offset and page number instead.
That layout cannot carry from the offset into the page in 32-bit
arithmetic, and LLVM folds pointer arithmetic freely, so here `_far`
pointers have the `_huge` layout. A `_far` pointer passed to or from
TASKING-built code is therefore not the same bits; `_huge` pointers are.

`_far` and `_huge` variables (not in the tiny model, as in TASKING), and in
the medium and large models every unqualified variable, can lie anywhere;
give their classes address ranges:

| Class | What | Section type |
|---|---|---|
| `CHUGE` | `_huge` variables (`C166_HDATA`, `C166_HBSS`) | `HDATA`, within one 64 KB segment |
| `CHUGEROM` | `_huge const`, initial values (`C166_HDATA_INIT`) | `HCONST` |
| `CFAR` | `_far` variables (`C166_FDATA`, `C166_FBSS`) | `DATA`, within one 16 KB page |
| `CFARROM` | `_far const`, initial values (`C166_FDATA_INIT`) | `CONST` |

`__cstart` fills and clears them as it does near variables. A named
variable is read and written directly, `EXTS #SEG x, #1` then
`MOV Rd, SOF x`.

An interrupt function saves the registers it uses on the user stack it
interrupted, and MDC/MDH/MDL when it multiplies, divides, calls a function
or has inline assembly, and returns with `RETI`. `_using(push)` asks for
the same frame.

With `_using(name)` it runs in a register bank of its own instead, as in
TASKING. The frame is shorter and saves no registers:

```
_isr?T  PROC    TASK
        MOV     DPP3:ISR_RB, R0     ; the bank's R0: the interrupted user stack
        SCXT    CP, #DPP3:ISR_RB
        NOP                         ; only if the next instruction uses a GPR
        ...
        POP     CP
        RETI
ISR_RB  REGDEF  R0-R15
```

The C167 cannot use a GPR in the instruction right after `SCXT CP`, hence
the `NOP`. The bank holds the registers the function uses, or R0-R15 if it
calls anything. Interrupt functions that name the same bank, in one module
or several, share it, so give them the same interrupt level. A function
that needs no register does not switch banks.

openc166 places banks as L166 does, at the lowest free even address from
`0FC00H` on. That is also where CP points after reset, and `__cstart` keeps
the CP it is called with. So keep the bank of the code that gets
interrupted out of the way with `--reserve`:

```sh
clang --target=c166 -Os main.c -o prog.hex -Wl,--reserve,0FC00h-0FC1Fh
```

In the stock 5WY19 firmware, the interrupts switch CP to banks from
`0FC2EH` up to `0FD06H`, and the bit-addressable RAM above that holds the
firmware's flags. A payload that keeps the firmware running therefore has
no free room for a bank: its interrupt functions should use `_using(push)`.

Intrinsics: `_nop`, `_srvwdt`, `_diswdt`, `_einit`, `_idle`, `_pwrdn`,
`_int166`, `_atomic`, `_rol`, `_ror`, `_prior`, `_mul32`, `_mulu32`,
`_div32`, `_divu32`, `_mod32`, `_modu32`, `_getbit`, `_putbit`, `_bfld`,
`_testset`, `_testclear`. The bit intrinsics take an SFR or a `_bitword`
(`_testset`/`_testclear`: an `_sfrbit` or `_atbit` bit) and use the bit
instructions, so they are atomic.

Inline assembly: `__asm__("...")` with `%0` operands; `"r"` gives a
register, `"i"(&var)` the A166 name of a variable or SFR.

## Not yet

- `_near` function pointers (all function pointers are far in the small
  and large models, near in tiny and medium).
- `#pragma` (`noframe`, `regdef`, ...), `_localbank`, `_packed`,
  `_noalign`, `_stackparm`.
- `_bit` variables as bits in the bit-addressable RAM.
- Alignment above 2 bytes (`_Alignas(4)`): A166 has only `EVEN`.
- In the C library: floating-point `printf` conversions, `scanf`, files,
  `time.h`, `signal.h`, `locale.h`.
