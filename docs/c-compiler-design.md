# openc166 — C Compiler: Design

Status: written 2026-11-27 as the design. Nothing is implemented yet.

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
| `float` | 32 | 16 | later, soft-float |
| `double` | 64 | 16 | later, soft-float; TASKING's 32-bit-double option is an open question (§9) |
| `_near` pointer | 16 | 16 | small model default |
| `_far`, `_huge`, `_shuge` pointer | 32 | 16 | later |
| function pointer | 16 (near code), 32 (far code) | 16 | small model: 16 |

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

### 3.4 Calling convention (TASKING C166) — to be confirmed

The convention below is TASKING's as best known before work starts. Step 1
of milestone 1 is to confirm it against the TASKING C166 manual and against
calls in the stock 5WY19 firmware in Ghidra; this table is then corrected.

| Item | Register / place |
|---|---|
| Arguments | first words in `R12`, `R13`, `R14`, `R15`; a `long` takes two consecutive registers, low word first; the rest on the user stack |
| Return value | `R4` (8/16-bit), `R4:R5` (32-bit, `R4` low), pointer to a caller buffer for structs |
| User stack pointer | `R0`, grows downwards; push is `MOV [-R0],Rn`, pop is `MOV Rn,[R0+]` |
| Callee-saved | to be confirmed (expected: `R6`–`R9`) |
| Caller-saved | the rest, `MDH`, `MDL`, `MDC`, `PSW` flags |
| Frame | locals at `[R0+#d16]`; no frame pointer unless needed (variable-length arrays, `alloca`) |
| Calls | `CALLA`/`CALLR` in the small model; `CALLS` and `RETS` for far code later |

Interrupt functions (`_interrupt(n)`) save every register they use plus
`MDH`/`MDL`/`MDC` when they multiply or divide, end in `RETI`, and get a
vector entry at `n*4`. `_using(bank)` switches register banks with `SCXT CP`
instead of saving registers, as TASKING does.

### 3.5 Instruction selection notes

- Natural 8- and 16-bit operations; 32-bit operations split into word pairs
  with `ADDC`/`SUBC`/`CMPC`-style sequences, and runtime calls for 32-bit
  multiply, divide and variable shifts.
- `MUL`/`MULU`/`DIV`/`DIVU`/`DIVL`/`DIVLU` go through `MDH`/`MDL`.
- Comparisons set `PSW`; branches use `JMPR cc_xx` and `JMPA` when out of
  range. Branch relaxation runs in the backend, so that openc166's generic
  `JMP` is not relied on to fix ranges.
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

All in assembly in this repository, assembled by openc166, linked as library
modules (`--lib`) so only what a program uses is pulled in:

- **startup** (`cstart.a66`): `SYSCON`/`BUSCON` set-up as the board needs,
  `DPP` set-up for the small model, system and user stack, copying
  initialised data, zeroing `.bss`, calling `main`. Payloads that are
  started by a loader can skip most of it.
- **helpers** for what the backend calls instead of inlining: 32-bit
  multiply, divide, modulo and shifts; later 64-bit and soft-float. Named
  as LLVM's libcalls, written for the TASKING convention.
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
- **Differential**, if a TASKING C166 compiler is available: compile the same
  sources with both and compare behaviour in the simulator (not bytes).

---

## 8. Milestones

1. **Ground work.** Confirm the calling convention (§3.4). Fork
   llvm-project, add an empty C166 target and Clang target info with the data
   layout; `clang --target=c166 -S` on an empty function prints a `RET` that
   openc166 assembles.
2. **Integer core, small model.** 8/16-bit arithmetic, loads and stores,
   branches, calls, locals on the user stack, the TASKING convention, line
   markers. The driver runs openc166. First execution tests pass.
3. **32-bit integers and runtime helpers.** `long`, multiply/divide via
   `MDH`/`MDL`, the helper library, startup code. Torture-test subset passes.
4. **TASKING extensions for payloads.** `_sfr`/`_sfrbit`, `_bit`,
   `_interrupt`/`_using`, `_at`, intrinsics, the register header. A
   flash-driver or MiniMon-style payload in C runs in the simulated 5WY19.
   **First usable release.**
5. **Code quality.** Short forms, post-increment addressing, bit
   instructions, branch shortening, comparison against TASKING output on
   real code.
6. **Full firmware.** Far/huge pointers and `EXTP`/`EXTS`, far code with
   `CALLS`/`RETS`, segmented memory models, `long long`, soft-float, libc.
7. **Later.** Variable-level debug information; an integrated assembler
   generated from `c166.tbl`; a Keil dialect and ABI mode.

---

## 9. Risks and open questions

- **Building and shipping LLVM.** A trimmed Clang/LLVM with one target is
  still a long build (tens of minutes on 32 cores) and a ~100 MB binary per
  platform. Prebuilt releases per OS from CI are needed; users should never
  have to build LLVM.
- **Rebasing the fork.** Clang changes for the TASKING keywords touch the
  parser and Sema. Keep them small and behind the C166 target so rebases on
  new LLVM releases stay cheap.
- **Calling convention details** are unconfirmed (§3.4): callee-saved set,
  how structs and `long` arguments that do not fit are passed, varargs.
- **`double` width**: TASKING C166 can make `double` 32-bit. Which default
  matches the code we want to build?
- **DPP assumptions** of the small model must match how openc166 and the
  startup code set the data page pointers; a mismatch fails silently at run
  time. The linker map and a startup check should catch it.
- **MDH/MDL in interrupts**: an interrupt that divides while the interrupted
  code divides needs `MDC`'s busy flag handled correctly, as TASKING does.
- **Is a TASKING C166 compiler available** for differential testing?
