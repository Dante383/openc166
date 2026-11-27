# openc166 — C Compiler: Design

Status: written 2026-11-27 as the design. Milestones 1-4 are done (2026-11-27); [c-compiler.md](c-compiler.md) is the user guide
in a local llvm-project fork, `openc166-llvm`, branch `c166`, based on LLVM
23.1.6; its `C166.md` says how to build it.

Scope: a C compiler for the C166/C167 family, built as an LLVM backend with a
Clang front end, that writes assembly for openc166 to assemble and link. The
first target is small programs for the Siemens 5WY19 ECU (C167CS-LM): RAM
payloads, flash drivers and patches to the stock firmware.

Decisions taken before this document:

| Question | Decision |
|---|---|
| How to get a compiler | An **LLVM backend** plus Clang target support. Not a new compiler, not an SDCC port. |
| C dialect | **C99 with TASKING C166 extensions** (`_near`, `_far`, `_huge`, `_interrupt()`, `_bit`, `_sfrbit`, ...), the toolchain gkflasher's MiniMon code and, most likely, the stock firmware come from. |
| Calling convention | **TASKING C166**, so compiled code can call and be called by TASKING-built code. |
| First usable version | **Payloads in the small model**: one code segment, near data, integers and pointers, no floating point. |

---

## 1. Goals and non-goals

**Goals**

- Compile C99 with the TASKING extensions that embedded C166 code actually
  uses, to code a reader of TASKING output would recognise.
- Call stock-firmware routines from C, and let stock code call C, without
  hand-written glue: same registers, same stack, same return values.
- Reuse what openc166 already has after code generation: the assembler,
  linker, section placement, HEX/S-record/ELF output, line markers, the
  simulator and the debugger. The compiler stops at assembly text.
- Source-level debugging of compiled C in the openc166 debugger and in
  Ghidra, through the line markers the assembler already understands.
- Cross-platform, like the rest of openc166: Linux, Windows, macOS.

**Non-goals, for now**

- Binary compatibility with TASKING object files or libraries (OMF-166).
  Interoperability is at the level of calling convention and source, not
  linking against TASKING `.obj`/`.lib` files.
- The Keil C166 dialect and ABI. A Keil mode could follow later as a second
  calling convention and a set of keyword aliases.
- Code size or speed parity with TASKING at the start. LLVM's optimiser is
  strong, but the C166-specific peepholes (short forms, `[Rw+]` addressing,
  bit instructions) come over time.
- C++, and a full hosted C library.

---

## 2. Architecture

```
  main.c ──► clang --target=c166 ──► main.a66 ──► openc166 ──► .bin/.hex/.elf
             (front end, LLVM IR,     (Keil syntax,  (assemble, link,
              C166 backend,            line markers)   place sections)
              AsmPrinter)                                    │
                                                             ▼
                                               openc166-sim / debugger / Ghidra
```

- **Clang**: parses C and the TASKING extensions, lays out types for C166,
  and lowers memory qualifiers to LLVM address spaces and attributes.
- **LLVM C166 backend** (`llvm/lib/Target/C166`): instruction selection,
  register allocation, frame lowering and the TASKING calling convention.
  It prints assembly; it has no integrated assembler or object writer at
  first.
- **openc166**: assembles the compiler's output with the runtime library
  and startup code, places sections and writes the image. The Clang driver
  calls it as the external assembler and linker, so
  `clang --target=c166 main.c -o main.hex` works in one step.

Why assembly text and not object files: openc166 already does section
placement, L166-style linking and DWARF line tables, and is checked byte for
byte against Keil. An LLVM MC object writer would duplicate that and need an
object format openc166 does not read. A later integrated assembler is
possible (§8), and could be generated from `src/isa/c166.tbl`.

### 2.1 Where the code lives

Clang targets cannot be loaded as plugins, so the backend and the Clang
changes live in a **fork of llvm-project**, in its own repository (proposed:
`openc166-llvm`), on a branch rebased on LLVM releases. This repository keeps
the assembler and gains only what the compiler needs from it:

- the runtime library and startup code, as assembly (§5);
- tests that compile C and run it in the simulator (§7);
- driver documentation.

The fork carries the C166 target as an experimental target
(`LLVM_EXPERIMENTAL_TARGETS_TO_BUILD=C166`) and builds with every other target
turned off, which keeps build time and binary size down. Licences are
compatible: LLVM is Apache-2.0 with the LLVM exception, openc166 is MIT.

---

## 3. The target as LLVM sees it

### 3.1 Data layout

| C type | Size | Alignment | Notes |
|---|---|---|---|
| `char` | 8 | 8 | unsigned by default, as in TASKING |
| `short`, `int` | 16 | 16 | |
| `long` | 32 | 16 | low word at the lower address |
| `long long` | 64 | 16 | later |
| `float` | 32 | 16 | soft-float |
| `double` | 64 (`-mdouble=32`: 32) | 16 | soft-float; see below |
| `_near` pointer | 16 | 16 | small model default |
| `_far`, `_huge`, `_shuge` pointer | 32 | 16 | later |
| function pointer | 16 (near code), 32 (far code) | 16 | small model: 16 |

**`double`**: the compiler specification says `double` and `long double`
are 64-bit IEEE binary64, as C99 expects. An explicit 32-bit mode
(`-mdouble=32`, Classic's `-Fs`) makes `double`
the same as `float`. The 32-bit mode is implemented first, because it needs
only the single-precision runtime and is what small ECU code uses; the
64-bit default follows in the full-firmware milestone. Until then the
compiler refuses to compile code that uses `double` without `-mdouble=32`,
rather than silently changing its meaning. The two modes are not
link-compatible, and the assembly output records which one was used.

Little-endian. Words need even addresses: the C167 traps on an odd word
access, so no 16-bit value is ever under-aligned. Proposed data layout string
for the small model:

```
e-m:e-p:16:16-i8:8-i16:16-i32:16-i64:16-f32:16-f64:16-a:8-n8:16-S16
```

### 3.2 Memory qualifiers as address spaces

| Qualifier | Address space | Pointer | Reaches |
|---|---|---|---|
| (default in small model), `_near` | 0 | 16-bit, through `DPP0-3` | 4 pages of 16 KB chosen at link time |
| `_far` | 1 | segment:offset, object within one page | whole 16 MB |
| `_huge` | 2 | 24-bit linear, object up to 64 KB | whole 16 MB |
| `_shuge` | 3 | 24-bit linear, object larger than 64 KB | whole 16 MB |
| `_system` | 4 | 16-bit, page 3 (`DPP3`) | SFRs and internal RAM |
| `_iram`, `_bita` | 4 + section attribute | as `_system` | internal RAM, bit-addressable RAM |

In the small model only spaces 0 and 4 exist; Clang rejects the others with a
clear error until they are implemented. The backend emits `EXTP`/`EXTS`
sequences for spaces 1–3 later.

### 3.3 Registers

- 16 word registers `R0`–`R15`; byte registers `RL0`–`RH7` are halves of
  `R0`–`R7` and exist only there. LLVM models this with a byte register class
  restricted to `R0`–`R7` as sub-registers.
- `R0` is the **user stack pointer** (TASKING): never allocated.
- The **system stack** (`SP`) holds only return addresses and what
  interrupt entry pushes. It is small (internal RAM) and is not used for
  locals or arguments.
- `MDH`/`MDL` (multiply/divide results) and `PSW` flags are modelled as
  physical registers that `MUL`, `DIV`, `CMP`, ALU instructions and
  conditional jumps define and use.

### 3.4 Calling convention (TASKING C166)

Confirmed on 2026-11-27 against the stock 5WY19 boot block in Ghidra, which
TASKING's Classic C166 compiler built, and against the TASKING C166/ST10
v8.9 C compiler manual (§3.6 Function Parameters, §3.15 Register Usage,
§7 stack layout; public at
<https://www.tasking.com/support/c166-classic/c166_v8.9.pdf>):

| Item | Register / place | Seen in |
|---|---|---|
| Arguments | first words in `R12`, `R13`, `R14`, `R15`, in order; a 32-bit argument (`long`, `_huge` pointer) takes two consecutive registers, low word first (`R12` offset, `R13` segment); the rest on the user stack | `BL_read_triple_redundant_block` (4 arguments, `_huge` pointer in `R12:R13` used with `EXTS R13`), `calculate_crc16_checksum`, `BL_compare_block_mirror_masked` |
| Arguments on the stack | an argument that no longer fits, any `float`/`double`, and any struct or union passed by value goes on the user stack, **and so does every argument after it**, even one that would fit a register (`f(long, int, long, char *)`: `R12:R13`, `R14`, stack, stack — `R15` stays unused). The `...` arguments of a variadic function and every argument of a `_stackparm` function go on the stack | manual §3.6 |
| Return value | `RL4` (`char`), `R4` (`int`, near pointer), `R4:R5` (`long`, `float`, far/huge/shuge pointer; `R4` offset, `R5` page or segment), `PSW.6`/`USR0` (`_bit`); `double` and structs: in a block on the user stack, `R4` points to it | manual Table 3-15; the functions above; the 32-bit division helper |
| User stack pointer | `R0`, grows downwards; push is `MOV [-R0],Rn`, pop is `MOV Rn,[R0+]`, frames dropped with `ADD R0,#n` | every function |
| Callee-saved | `R6`, `R7`, `R8`, `R9`, saved only when used | manual §3.6.1; `BL_read_triple_redundant_block` saves all four; others save `R6` or `R9` alone |
| Parameter registers in the callee | a leaf function keeps its parameters in `R12`–`R15` and may use the free ones for locals; a non-leaf function stores them on the user stack at entry "as if pushed by the caller" | manual §3.6; `BL_load_verify_redundant_blocks` pushes `R13`, `R12` first |
| Caller-saved | `R1`–`R5`, `R10`–`R15`, `MDH`, `MDL`, `MDC`, `PSW` flags | callees change `R3`, `R4`, `R5`, `R10`–`R15` without saving; callers reload `R12`–`R15` before each call and keep values across calls in `RL6` |

Runtime helpers do not follow this convention: the 32-bit unsigned division
at `002014H` takes its operands in `R4:R5` and `R10:R11` and returns in
`R4:R5`. The backend calls such helpers with their own register conventions,
named after TASKING's helper names where they can be identified in the
firmware. The manual documents the floating-point helpers' registers
(§3.16). Still open: the order of stack arguments, which the manual's stack
diagram (§7) shows only in outline; to be settled with a test program once
a Classic compiler runs, or from calls in the firmware.
| Frame | locals at `[R0+#d16]`; no frame pointer unless needed (variable-length arrays, `alloca`) |
| Calls | `CALLA`/`CALLR` in the small model; `CALLS` and `RETS` for far code later |

The boot block mixes near functions (`CALLA` … `RET`) and far ones (`RETS`)
in one program, so TASKING's medium/large-model code is what the far-code
milestone has to match.

Interrupt functions (`_interrupt(n)`) save every register they use plus
`MDH`/`MDL`/`MDC` when they multiply or divide, end in `RETI`, and get a
vector entry at `n*4`. `_using(bank)` switches register banks with `SCXT CP`
instead of saving registers, as TASKING does.

### 3.5 Instruction selection notes

- Natural 8- and 16-bit operations; 32-bit operations split into word pairs
  with `ADDC`/`SUBC`/`CMPC`-style sequences, and runtime calls for 32-bit
  multiply, divide and variable shifts.
- `MUL`/`MULU`/`DIV`/`DIVU`/`DIVL`/`DIVLU` go through `MDH`/`MDL`.
- Comparisons set `PSW`; branches are openc166's generic `JMP cc_xx`,
  which becomes `JMPR` or `JMPA` by distance, so the back end does no
  branch relaxation (decided in milestone 2).
- `BSET`/`BCLR`/`BMOV`/`JB`/`JNB`/`JBC` for `_bit`, `_sfrbit` and
  single-bit tests on bit-addressable memory.
- The C166 short forms (`[Rw0-3]`, `[Rw0-3+]`, `#data3`, `#data4`) are chosen
  by openc166 from generic mnemonics where it can; the backend prefers the
  registers that make them possible.
- `ATOMIC`/`EXTR` sequences are kept as bundles that the scheduler and
  register allocator cannot split.

---

## 4. The TASKING extensions

| Extension | Clang | Backend |
|---|---|---|
| `_near`, `_far`, `_huge`, `_shuge`, `_system`, `_iram` | keywords → address space qualifiers | address spaces (§3.2) |
| `_bit`, `_bitword` | new built-in types; `_bit` is 1 bit in the bit-addressable area | bit sections (`BIT`/`DBIT`) and bit instructions |
| `_sfr`, `_sfrbit`, `_esfr`, `_esfrbit` | declarations bound to a fixed address | `reg` and bit-address operands; `EXTR` for ESFRs |
| `_interrupt(n)`, `_using(bank)` | function attributes | prologue/epilogue, vector entry |
| `_inline`, `_noinline` | aliases of `inline` and the attribute | — |
| `_at(addr)` | variable at a fixed address | `SECTION ... AT` |
| intrinsics (`_nop()`, `_srvwdt()`, `_diswdt()`, `_einit()`, `_idle()`, `_atomic()`, `_rol()`, `_ror()`, `_mul32()`, `_getbit()`, `_putbit()`, ...) | builtins | single instructions or short sequences |
| `#pragma` (`class`, `section`, `regdef`, ...) | parsed; unknown ones warn | section names and classes |

The register header TASKING code includes (`reg167.h` / `regc167cs.h`) is
written from Infineon's manuals for openc166, as `REG167.INC` was. The list
above is ordered by what MiniMon, gkflasher payloads and typical ECU code
use; the rest follows on demand.

---

## 5. Runtime and startup

In this repository (`runtime/`), assembled by openc166, linked as library
modules (`--lib`) so only what a program uses is pulled in:

- **startup** (`cstart.a66`): user stack, copying initialised data,
  zeroing `C166_BSS`, calling `main`, returning its result (so a loader or
  firmware can call a C program). `SYSCON`/`BUSCON`/`DPP`/system stack
  set-up for programs that start from reset is left to the board.
- **helpers** for what the backend calls instead of inlining: 32-bit
  multiply, divide, modulo and shifts; later 64-bit integers and soft-float
  (single precision first, then double). Their register use follows
  TASKING's own helpers where the firmware shows them (§3.4).
- **libc**: only `mem*`/`str*` at first. A small libc (picolibc, or
  newlib-nano's subset) comes after the full-firmware milestone.

---

## 6. Debug information

- The AsmPrinter writes a line marker (`# 42 "main.c"`) whenever the source
  line changes. openc166 already carries markers into its JSON debug
  information and the ELF's DWARF line table, so stepping by C line works in
  the openc166 debugger and in Ghidra without new assembler work.
- Variables, types and frame layout are a second step: the compiler passes
  them to openc166 (as assembler directives or a sidecar file) and openc166
  writes them into DWARF. The format is decided when the line-level path
  works.

---

## 7. Testing

- **LLVM lit tests** in the fork for instruction selection, the calling
  convention, frame lowering and every extension.
- **Assembly round trip**: every lit test's output assembles with openc166
  without errors or warnings. Catches syntax the backend gets wrong.
- **Execution tests** here: C programs compiled, assembled, run in the
  simulator, results checked through the Python API. The simulator is the
  test oracle; it is already checked against the real 5WY19 firmware.
- **Conformance**: a subset of LLVM's test-suite `SingleSource` and the GCC
  torture tests (`gcc.c-torture/execute`), run in the simulator, filtered to
  what the current milestone supports.
- **ABI**: call stock-firmware routines from C in the simulated 5WY19, and
  call C from hand-written TASKING-convention assembly.
- **Differential**, if a TASKING Classic C166 compiler is available: compile the same
  sources with both and compare behaviour in the simulator (not bytes).
- **Keil C166 as a behavioural oracle.** Keil C166 V7.57 (eval) runs under
  Wine. Its dialect and calling convention differ from TASKING's (arguments
  from `R8`, e.g. `MOV R4,R8 / ADD R4,R9`), so it cannot check the ABI or
  the TASKING extensions. It can compile portable C to assembly that
  openc166 assembles, so the same test program built by both compilers must
  behave the same in the simulator. The Keil runtime library (`?C_LMUL`,
  ...) is not ours to ship; tests link it from the local Keil installation
  only.

---

## 8. Milestones

1. **Ground work.** Fork
   llvm-project, add an empty C166 target and Clang target info with the data
   layout; `clang --target=c166 -S` on an empty function prints a `RET` that
   openc166 assembles.
   *Done.* Beyond the empty function, register arguments, results in `R4`,
   16-bit moves, constants and `ADD`/`SUB`/`AND`/`OR`/`XOR` work; compiled
   functions give the right results in the simulator. Settled on the way:
   - Output starts with `$NOMACRO` (as Keil C166's does: LLVM's comments
     contain `%`, which would start an MPL call) and `$CASE`.
   - Sections are named as TASKING names them, from the source file:
     `main.c` → `MAIN_PR SECTION CODE WORD PUBLIC 'CPROGRAM'`.
   - C names get a leading underscore (`add` → `_add`), as in TASKING;
     compiler-made labels start with `?L`.
   - The only change to LLVM's shared code is a hook in `MCAsmInfo` for
     printing alignment (A166 has `EVEN`, not `.p2align`).
   - `-O0` does not compile yet: unoptimised code keeps arguments in stack
     slots, and stack frames are milestone 2.
2. **Integer core, small model.** 8/16-bit arithmetic, loads and stores,
   branches, calls, locals on the user stack, the TASKING convention, line
   markers. The driver runs openc166. First execution tests pass.
   *Done.* `python/tests/test_c_compiler.py` compiles C at -O0 and -O2,
   links it with openc166 and runs it in the simulator (skipped when no
   C166 clang is found; `OPENC166_CLANG` points at one). Decided on the way:
   - **An object file is A166 source.** Asked for an object, the back end
     writes assembly; openc166 links modules as source, as L166 links
     objects. So `clang -c` and `clang a.c b.c -o prog.hex` work, with
     openc166 as the linker, and no object format is needed.
   - **Compare and branch, and multiply/divide, are single instructions
     until printing.** Every data move on the C166 sets N and Z, so a
     register copy between `CMP` and `JMP cc` would break the branch; kept
     whole, nothing can come between. The same holds for `MUL`/`DIV` and
     reading `MDL`/`MDH`.
   - **Generic `JMP` and `CALL`.** openc166 picks `JMPR`/`JMPA` and
     `CALLR`/`CALLA` by distance, so the back end does no branch
     relaxation of its own (a change from §3.5).
   - **Bytes use the low half of a word register** (`R5` printed as `RL5`)
     instead of an 8-bit register class: `i8` stays an illegal type that is
     promoted to 16 bits, and only `R0`-`R7` can be used for byte access.
   - **TASKING's stack rules are in the argument assignment**: once one
     argument is on the stack all later ones are, a `long` never starts in
     `R15`, `...` arguments are on the stack. Stack arguments are laid out
     first argument lowest, starting at `R0` on entry; that order is still
     to be checked against Classic.
   - Each global gets its own section internally, mapped back onto the
     module's A166 sections, which keeps ELF's `.comm` out of the output;
     compiler-made names with `.` (`.str`, `f.counter`) are renamed with
     `?`.
3. **32-bit integers and runtime helpers.** `long`, multiply/divide via
   `MDH`/`MDL`, the helper library, startup code. Torture-test subset passes.
   *Done*, except the torture tests (the GCC test suite is not fetched
   yet); `test_c_compiler.py` covers `long` arithmetic, the runtime and
   the startup code in the simulator. Decided on the way:
   - `ADD`+`ADDC` and `SUB`+`SUBC` are single pseudo instructions until
     printing, like compare and branch: the carry is in the flags.
   - 32-bit multiplication is inline (`MULU` for the low halves, `MUL` for
     the cross products); division, remainder and variable shifts call
     `__divsi3`, `__udivsi3`, `__modsi3`, `__umodsi3`, `__ashlsi3`,
     `__lshrsi3`, `__ashrsi3` in the runtime, with the normal calling
     convention.
   - The runtime is C compiled by the C166 clang (`runtime/src`, built to
     `runtime/lib` by `runtime/build.py` and committed), linked as library
     modules. `runtime/cstart.a66` is the startup code. The driver links
     both unless `-nostartfiles`/`-nodefaultlibs`/`-nostdlib`.
   - Variables: all modules share `C166_DATA` (initialised, RAM),
     `C166_DATA_INIT` (their values, ROM) and `C166_BSS` (cleared). openc166
     combines same-named sections in module order, so `__cstart` copies
     `C166_DATA_INIT` onto `C166_DATA` in one go using
     `?SECTION_START?`/`?SECTION_SIZE?`. RAM sections hold only `DSB`, so
     the image (and a HEX file) has no bytes at RAM addresses.
   - No `DD`: A166 aligns it to 4 bytes, which would shift the ROM copy
     against the RAM layout; 32-bit values are written as two `DW`.
   - Not yet: `long long` (milestone 6). A packed struct with a word at an
     odd offset would be padded by `DW`'s alignment.
4. **TASKING extensions for payloads.** `_sfr`/`_sfrbit`, `_bit`,
   `_interrupt`/`_using`, `_at`, intrinsics, the register header. A
   flash-driver or MiniMon-style payload in C runs in the simulated 5WY19.
   **First usable release.**
   *Done.* A MiniMon-style payload in C (ASC0 set up from C, commands over
   the K-line, CRC-16 of flash ranges, watchdog serviced) runs from the
   5WY19's internal RAM in the simulator and returns to its caller.
   Decided on the way:
   - **The keywords are macros Clang predefines**, built on what Clang
     already has: `_sfr` is an `extern volatile unsigned int` in section
     `.c166.sfr`, `_sfrbit` a `_Bool` in `.c166.bit`, `_atbit(P2, 3)` the
     asm label `P2.3`, `_at()` and `_interrupt()` sections. The back end
     reads these marks (`C166Globals.h`). Clang itself only gained the
     macros, the headers and the C166 target info, so rebases stay cheap.
   - **SFRs and SFR bits are used by name**, as Classic does: the compiler
     writes `P2`, `IEN`, `P2.3` and openc166 takes the addresses from
     `REG167.INC`, which every module includes (`$MOD167`,
     `$INCLUDE(REG167.INC)`). `<reg167.h>` is generated from that file
     (`tools/reg_h.py`), so the two cannot disagree.
   - **Interrupt frames push** (Classic's `_using(push)`): used registers on
     the user stack of the interrupted code, MDC/MDH/MDL on the system stack
     when needed, `RETI` inside `PROC TASK`, a `JMPS` vector at 4 x n.
     Register bank switching is not done yet.
   - **Intrinsics are a header** (`<c166.h>`): inline assembly and Clang's
     generic builtins, no new Clang builtins. Inline assembly takes `"r"`
     and `"i"(&var)` operands; the latter prints a variable's A166 name.
   - `_bit` variables are bytes (`_Bool`), not bits in the bit-addressable
     RAM; `_sfrbit`/`_atbit` bits are real bits.
   - LLVM gives large initialised globals 16-byte alignment on its own;
     A166 has only `EVEN`, and the C166 never needs more, so any alignment
     above 2 prints `EVEN`.
5. **Code quality.** Short forms, post-increment addressing, bit
   instructions, branch shortening, comparison against TASKING output on
   real code.
6. **Full firmware.** Far/huge pointers and `EXTP`/`EXTS`, far code with
   `CALLS`/`RETS`, segmented memory models, `long long`, soft-float
   (single precision and `-mdouble=32` first, then 64-bit `double` as the
   default), libc.
7. **Later.** Variable-level debug information; an integrated assembler
   generated from `c166.tbl`; a Keil dialect and ABI mode.

---

## 9. Risks and open questions

- **Building and shipping LLVM.** A trimmed Clang/LLVM with one target is
  still a long build (tens of minutes on 32 cores). Decided: CI publishes
  prebuilt releases of about 100 MB per platform (Linux, Windows, macOS);
  users never build LLVM.
- **Rebasing the fork.** Clang changes for the TASKING keywords touch the
  parser and Sema. Keep them small and behind the C166 target so rebases on
  new LLVM releases stay cheap.
- **Calling convention details**: settled by the Classic manual (§3.4)
  except the order of stack arguments.
- **DPP assumptions** of the small model must match how openc166 and the
  startup code set the data page pointers; a mismatch fails silently at run
  time. The linker map and a startup check should catch it.
- **MDH/MDL in interrupts**: an interrupt that divides while the interrupted
  code divides needs `MDC`'s busy flag handled correctly, as TASKING does.
- **A TASKING C166 compiler for differential testing.** Both TASKING C166
  products are now legacy, and neither has a public download:
  - *C166/ST10 Classic* (v8.x and earlier): the compiler that built the
    stock firmware, and the one whose convention and output we match. Its
    manuals (v6.0–v8.9) are public on tasking.com, and the v8.6r1 patch 3
    there contains `c166.exe`, `a166.exe` and `l166.exe`, which run under
    Wine but stop with a FLEXlm licence error without a TASKING licence.
    Wanted for code comparison; needs a licence or evaluation from
    TASKING support.
  - *VX-toolset for C166* (v3.1r2, 15-day trial, installed under Wine on
    2026-11-27): **not a reference for the ABI.** Its user guide (§21.6.4)
    says its calling convention is "completely different" from Classic's,
    and that it cannot call Classic-compiled code in ROM or flash. Checked:
    VX passes arguments in `R2`–`R5`, then `R11`–`R14` (a 32-bit argument
    starts at an even register), returns in `R2`/`R2:R3`, and keeps the user
    stack in `R15`; Classic uses `R12`–`R15`, `R4`/`R4:R5` and `R0`. VX also
    spells the extensions with two underscores (`__huge`) and writes a
    different assembly syntax (`.section`, `movw`).

  VX is still useful: its user guide's migration chapter (§21.6.2–21.6.7)
  lists every Classic keyword, pragma, memory model and option with its
  meaning, which is the best dialect reference available, and VX works as a
  behavioural oracle for portable C, like Keil.
