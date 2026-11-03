# openc166 — Phase 1: Feasibility Assessment, Roadmap & Recommendation

Scope: an open-source, cross-platform, Keil A166-compatible assembler for the
Infineon C166/C167 family, producing raw binary images of small ECU payloads at
explicit load/execution addresses, for integration into the Python-based OpenGK
(gkflasher).

Status: research only — no implementation.

> **Later decision (implementation):** the Python package talks to the C API
> through **ctypes** instead of a nanobind module. The package is plain
> Python plus the shared library, so one wheel per platform serves every
> Python 3 version. The C API that was built is
> [include/openc166/openc166.h](../include/openc166/openc166.h); it differs
> from the sketch in §5.3.

---

## 1. Executive summary

| Question | Answer |
|---|---|
| Is it feasible? | **Yes, low technical risk.** The C167 ISA is small and regular (84 mnemonics, ~236 opcode bytes, every instruction is 2 or 4 bytes). The hard part is reproducing Keil A166's *type-driven* operand semantics, which can be tested empirically. |
| Can we test against Keil? | **Yes, verified.** Keil A166 V5.38, L166 V5.29 and OH166 V4.8 (eval) run under Wine 9 on Linux. The `.LST` output gives exact bytes per source line and serves as a usable oracle. |
| Is there an independent open-source oracle? | **Yes, verified.** ASL (`code166.c`) was built locally. A differential pilot of **142 hardware-instruction forms produced 142/142 byte-identical results** against Keil A166. |
| Adapt an existing assembler? | **No.** None accepts Keil syntax or semantics (`cc_XX`, `SECTION … AT`, `PROC`, `ASSUME`/`DPPx:`, typed symbols, `$`-controls, generic `JMP`/`CALL`/`MOV`). ASL is the only credible base, but it is a monolithic multi-CPU C codebase under GPL-2.0-only OR GPL-3.0-only with no library API. |
| Recommendation | **Build a new, clean-room assembler: a C++20 core with a stable C API, a standalone CLI and Python wheels** (nanobind), structured around a declarative instruction table. This serves the whole C166 community, not only OpenGK. Use ASL and Keil A166 (Wine) as *external* differential oracles, and Ghidra SLEIGH (MIT) as a round-trip disassembler. |
| What gkflasher actually needs | gkflasher's bootstrap, kernel and flash drivers are Infineon **MiniMon** code written in the **TASKING** A166 dialect, not Keil (§6.1). Tier 1 therefore adds a small TASKING subset (§4.1, Tier 1b), so that these payloads can be rebuilt and modified from source. |
| Effort (Tier 1) | About **13–15 person-weeks** to a production-quality 0.1 release, including binaries and wheels for all three OSes and the TASKING subset that gkflasher's existing payloads need (see §9). |

---

## 2. Existing open-source assemblers

| Tool | C166 support | Syntax vs Keil | License | Verdict |
|---|---|---|---|---|
| **ASL / AS** (A. Arnold), `code166.c`, ~2.2 kLOC, actively maintained | 80C166, 80C167, 80C167CS. Covers the full ISA, ASSUME DPP, EXTR/EXTP/EXTS tracking, and automatic short/long JMP/CALL. **Built and verified locally.** | Incompatible: <ul><li>bare conditions (`jmpr nz,x`), not `cc_NZ`;</li><li>`[R2+4]`, not `[R2+#4]`;</li><li>its `SECTION` only scopes symbols;</li><li>no `PROC`, `DEFR`, typed symbols or `$`-controls;</li><li>numeric SFR addresses are auto-shortened to the `reg` form, which Keil rejects.</li></ul> | GPL-2.0-only OR GPL-3.0-only | **Best oracle.** A poor base for Keil compatibility. |
| **GNU binutils xc16x** (KPIT, 2006, CGEN) | XC16x, GAS syntax | Incompatible | GPL-3.0+ | Moved to obsolete, then **deleted in 2022** (binutils patch "drop XC16x bits"). Reference only. |
| **vasm** `cpus/c16x` (v0.2c, 2002–05) | C16x/ST10 | Partial `cc_` support; not Keil directives | **Non-commercial license** (commercial use needs written consent) | Avoid. Do not read the source, to keep the clean room. |
| **Ghidra c166 module** (keyhana, SLEIGH) | Disassembler/decompiler | N/A | MIT | Useful as a **round-trip decode oracle**. |
| naken_asm, customasm, rizin, LLVM (`llvm-c166`, experimental) | None, or immature | — | — | Not useful. |
| gkflasher `flasher/mtos_asm.py` | ~30 hand-encoded instructions; rel8 branches only | Method-call DSL | GPL-3.0 | **Integration target to replace.** |

There is no open-source Keil A166-syntax assembler. Only Keil and TASKING are
complete, and both are proprietary.

---

## 3. C167 instruction encoding requirements (from the Infineon ISM v2.0)

### 3.1 Inventory

- **84 hardware mnemonics:** 78 on the 8xC166, plus 6 C167-only (`ATOMIC`, `EXTR`, `EXTP`, `EXTPR`, `EXTS`, `EXTSR`). The C167 sub-ops share opcode bytes D1, D7 and DC.
- **About 236 defined opcode bytes.** All instructions are 2 or 4 bytes long, and 16-bit operands are little-endian.
- **Keil generic mnemonics** resolve by operand type and distance:
  - `JMP` becomes `JMPR`, `JMPA`, `JMPI` or `JMPS`.
  - `CALL` becomes `CALLR`, `CALLA`, `CALLI` or `CALLS`.
  - `RET` becomes `RET`, `RETS` or `RETI`, depending on the `PROC` type.
  - `MOV`, `ADD`, etc. select the byte (`…B`) or bit (`BMOV`, `BAND`, …) form from the operand types.

### 3.2 Operand classes the encoder must model

| Class | Encoding |
|---|---|
| `Rw` / `Rb` | 4-bit field. Byte registers are numbered RL0=0, RH0=1, … RH7=15. |
| `reg` (8 bit) | 00–EF → SFR at `FE00+2r`, or ESFR at `F000+2r` inside an EXTR window. F0–FF → GPR. |
| `mem` (16 bit) | Bits 15–14 select DPP0–3; bits 13–0 are the page offset. |
| Indirect | `[Rw]`, `[Rw+]`, `[-Rw]`, `[Rw+#data16]`. The short ALU forms use `[Rw0-3]` and `[Rw0-3+]`, encoded as `nnnn.10ii` / `nnnn.11ii`. |
| Immediates | `#data3` (`nnnn.0###`), `#data4`, `#data8`, `#data16`, `#mask8` |
| `bitoff` | 00–7F → `FD00+2b`; 80–EF → `FF00+2(b&7F)` (`F100…` under EXTR); F0–FF → GPR. Combined with a bit position to form `bitaddr`. |
| Branch targets | `rel`: signed 8-bit **word** displacement from the next instruction. `caddr`: 16-bit, even. `seg`: 8 bits. `cc`: 4 bits (UC=0, NET=1, Z/EQ=2, NZ/NE=3, V=4, NV=5, N=6, NN=7, C/ULT=8, NC/UGE=9, SGT=A, SLE=B, SLT=C, SGE=D, UGT=E, ULE=F). |
| Special | `#trap7` (encoded as `t<<1`), `#irang2` (count−1), `#pag10`, `#seg8` |

### 3.3 Encoding gotchas (all verified by Keil, ASL and the ISM)

- **Field order often differs from operand order:**
  - `BFLDL` = `0A QQ @@ ##` (mask, then data), but `BFLDH` = `1A QQ ## @@` (data, then mask).
  - `MOV [R2+#4],R1` = `C4 12 0400`, with the source register in the high nibble.
  - Bit-to-bit ops (`BMOV`, `BAND`, …) are encoded as `op QQ ZZ qz`.

  So encoding has to be specified **per form**; it cannot be inferred from operand order.
- **Bit number inside the opcode or fourth byte:** `BSET/BCLR bitoff.n` puts the bit number in the opcode (`(n<<4)|0x0F` or `|0x0E`). `JB/JNB/JBC/JNBS` use `op QQ rr q0`.
- **Short forms are chosen automatically:**
  - `MOV Rw,#data4` → `E0`
  - `MOVB Rb,#data4` → `E1`
  - ALU `Rw,#data3`, e.g. `ADD R1,#5` → `08 15`
- **Fixed 4-byte protected instructions:** `SRST`, `IDLE`, `PWRDN`, `SRVWDT`, `DISWDT`, `EINIT`, e.g. `SRST` = `B7 48 B7 B7`.
- **`MOVB reg,#data8` = `E7 RR ## xx`:** 4 bytes, with a don't-care byte that must match Keil (00).
- **Extended sequences:** `EXTP/EXTS/EXTR` open a 1–4 instruction window that changes how `reg`, `bitoff` and `mem` are interpreted. The assembler must track it, as Keil does (A110/A111).

### 3.4 Family variants

The default target is C167 (C167CR/CS, ST10F2xx-compatible). The 8xC166
subset is `$NOMOD166`/`$MOD166`-style gating: Keil raises A108 for C167
instructions used without `$MOD167`.

Deferred: the XC16x / C166S-V2 extended ISA, and the ST10 MAC instructions.

---

## 4. Keil A166 behaviour that must be replicated

These behaviours were verified empirically under Wine with independently
written probes.

| # | Behaviour | Evidence |
|---|---|---|
| 1 | **Numeric addresses are always `mem`.** The `reg` short form is used only for typed SFR/DEFR/GPR symbols. | `MOV 0FE12H,R1` → `F6F112FE`. `MOV 0FE12H,#1234H` → **error A74**, whereas ASL accepts it as `E609…`. |
| 2 | **SFR-to-GPR moves use the `mem,reg` form.** | `MOV DPP1,R1` → `F6F102FE`. `MOV DPP0,#x` → `E600…` (`reg` form). |
| 3 | **ESFR access is checked against EXTR windows.** | `MOV ODP2,#1` outside EXTR → warning A111, encoded `E6E1…` |
| 4 | **Multi-pass branch relaxation.** | <ul><li>Forward or backward target in range → `JMPR`/`CALLR` (2 bytes).</li><li>Out of range → `JMPA`/`CALLA`.</li><li>External target → `JMPA` plus a fixup.</li><li>`JMP cc_NZ,0E000H` → `EA30…`.</li></ul> |
| 5 | **A label's type comes from the next statement.** | `lbl: DS 300` makes `lbl` a data label, so `JMP lbl` → warning A74. |
| 6 | **`PROC` type drives `RET`.** | <ul><li>`RET` in a `PROC NEAR` → `CB00`.</li><li>`RETS` in a non-FAR procedure → A78.</li><li>`RETI` outside an `INTERRUPT` → A80.</li></ul> |
| 7 | **DPP-encoded data addresses in SEGMENTED mode.** | <ul><li>With `ASSUME DPP2:D1`, a label at `E000` encodes as `A000`, including as the immediate `#BYTE PTR VAR1`.</li><li>`DPP1:FCR`, with `FCR` = 3FFE, gives `7FFE`.</li></ul> |
| 8 | **Address operators.** | For `12345H`: `PAG`=4, `POF`=2345, `SEG`=1, `SOF`=2345. `HIGH`/`LOW` behave as expected. `'AB'` = 4142H. `$` is the per-section location counter. |
| 9 | **NONSEGMENTED truncation.** | `MOV R1,12345H` → `…4523` plus warning A62 |
| 10 | **`ORG` inside an `AT` section is absolute.** | `ORG 100H` in a section at `C000H` → error A132 |
| 11 | **Data layout.** | `DB` packs bytes. `DW` after an odd location was observed starting on an even address (to confirm). Gaps are not filled in the HEX output, so a fill policy is needed for raw binaries. |
| 12 | **Conditional assembly.** | `SET` redefinition and statement-level `IF` work. `$IF` uses only `$SET`/`$RESET` symbols. |
| 13 | **TASKING keywords are not Keil.** | `RETN` → syntax error A6 |

### 4.1 Feature tiers, based on corpus statistics (271 files)

The corpus combines MiniMon drivers with Keil board and startup files.
Statement frequency, from most common: EQU, MOV, BIT, SET, DEFR, JMPR, RET, ADD,
CALL, SUB, PUBLIC, EXTS, JMP, MOVB, … Operand features in use: `.n` bit
addressing (~10k uses), `cc_` (~3k), `DPPx:` (848), `[Rn+]` (525),
`PAG`/`POF`/`SEG`/`SOF`/`BOF` (418), and `HIGH`/`LOW` (114).

**Tier 1 — required for real payloads (MVP)**

- **Instructions:** all hardware mnemonics, plus Keil generic `JMP`/`CALL`/`RET`/`MOV`/`ADD`/… resolution; `cc_XX` symbols; the `.bit` operator.
- **Numbers:** H/D/O/Q/B suffixes, `$` digit separators, character constants.
- **Expressions:** the full Keil operator set and precedence: `PAG`, `POF`, `SEG`, `SOF`, `BOF`, `HIGH`, `LOW`, `BYTEn`, `WORDn`, `DATAn`, `PTR`, `SHORT`, `DPPx:`, `NOT`, `SHL`, `SHR`, `MOD`, signed and unsigned comparisons, C-style aliases.
- **Symbol definitions:** `EQU`, `SET`, `LIT`, `BIT`, `DEFR`, `DEFB`, `DEFA`, `DBIT`, typed labels, `LABEL`, `label: NEAR`.
- **Sections and procedures:**
  - `SECTION … [AT addr]` / `ENDS`, including reopening and nesting;
  - `PROC [NEAR|FAR|TASK|INTERRUPT]` / `ENDP`;
  - `ASSUME`, `ORG`, `EVEN`, `END`.
- **Data:** `DB`, `DW`, `DD`, `DS`, `DSB`, `DSW`, `DSD`, `DBPTR`, `DPPTR`, `DSPTR`.
- **Controls:** `$MOD167`, `$MOD166`, `$NOMOD166`, `$SEGMENTED`, `$NONSEGMENTED`, `$CASE`, `$INCLUDE` with an include path, `$SET`, `$RESET`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF`; statement-level `IF`/`ELSEIF`/`ELSE`/`ENDIF`.
- **Tolerated** (accepted, with no or minimal effect): `NAME`, `PUBLIC`, `GLOBAL`, `EXTERN`/`EXTRN` (resolved from user-supplied definitions), `REGDEF`/`REGBANK`/`SSKDEF`, `CGROUP`/`DGROUP`, `TYPEDEC`, `$TITLE`, `$LIST`/`$NOLIST`, `$EJECT`, `$MODV2`.
- **EXTx windows:** tracking plus diagnostics A110/A111.
- **Placement:** a **mini-locator** that places relocatable sections at user-given addresses (the L166 `ADDRESSES(SECTIONS(...))` equivalent, or TASKING's `AD (SE (name (addr)))`).
- **Multi-file builds without object files:** assemble several sources in one run into one image, resolving `PUBLIC`/`GLOBAL` against `EXTERN` across files. The gkflasher bootstrap loader and kernel are two such files.

**Tier 1b — TASKING subset (needed for gkflasher's existing payloads)**

gkflasher's own ECU-side code is MiniMon code written for the TASKING toolchain (verified in §6.1). Both Keil and TASKING named their assembler "A166", but they spell some things differently. Rebuilding gkflasher's payloads from source needs only this subset:

- the `RETN` and `RETV` keywords;
- the `EXTEND` and `NOMOD166` controls, and `STDNAMES(file)` (a register-name file, TASKING's equivalent of `REG167.INC`);
- `label: NEAR`, and `REGDEF R0-R15 private`;
- the `@SET (name,value)` / `@IF (@name)` / `@ELSE` / `@ENDI` subset of the M166 preprocessor;
- section placement in the style of TASKING `.loc` files.

**Tier 2**

- The full TASKING M166 macro language.
- Simple `MACRO`/`ENDM`, `REPT`, `IRP`.
- `PECDEF`, `DF32`/`DF64`.

**Deferred**

- The Keil MPL `%` macro language
- OMF-166 objects, relocation records, multi-module linking
- XC16x / C166S-V2 extended instructions

---

## 5. Proposed architecture

**Stack: C++20 core + stable C API + standalone CLI + Python wheels.**

### 5.1 Why a compiled core rather than pure Python

The project should serve everyone working with C166, not only OpenGK. Pure
Python is the fastest to write and is trivially cross-platform, but it limits
reach:

| Need | Pure Python | C++ core + C API |
|---|---|---|
| Drop-in A166 replacement in makefiles and CI | Needs an interpreter, or a ~10 MB bundled exe (often flagged by Windows antivirus) | One small static binary per OS |
| Embedding in C#, Java (Ghidra), C/C++ or Rust tools | Subprocess only | The C ABI is callable from every language |
| Python users (OpenGK, IDA, Binary Ninja, PyGhidra) | Native | Wheel via nanobind; same API shape |
| Browser playground | Pyodide | Emscripten/WASM |
| Development cost | Lowest | About +3–4 weeks (bindings, packaging, build matrix) |

C++20 was chosen over Rust because the repo already has a CMake/C++20
skeleton, and the C166 embedded community is C/C++-literate.

### 5.2 Layout

```
openc166/
  CMakeLists.txt          CMake ≥ 3.20. Targets: libopenc166 (static + shared),
                          openc166 (CLI), tests, optional Python module
  include/openc166/
    openc166.h            Stable C API: opaque handles, versioned, no
                          exceptions across the boundary
    openc166.hpp          Header-only C++ wrapper (RAII) over the C API
  src/
    source.*        Source files, include stack, line mapping (diagnostics/listing)
    lexer.*         Keil tokens: suffixed numbers, strings/chars, $controls, cc_, DPPx:
    preproc.*       $INCLUDE/$IF/$SET/$RESET, primary controls, include search path
    parser.*        Statement AST: [label[:]] mnemonic|directive operands ; comment
    expr.*          Pratt parser with Keil precedence levels 0–9
                    Typed values: (value, type, section, dpp, external?)
    symbols.*       Typed symbol table: NUMBER, GPR, SFR, ESFR, BIT, BYTE, WORD,
                    NEAR, FAR, DATAn, INTNO, SECTION, GROUP, REGBANK; CASE handling
    sections.*      Section model (AT or relocatable), per-section $ counter,
                    ORG/EVEN/DS, overlap checks
    dpp.*           ASSUME state, DPPx: overrides, SEGMENTED/NONSEGMENTED rules,
                    EXTP/EXTS/EXTR window tracking
    isa/c166.tbl    ONE declarative plain-text table: mnemonic, operand-class
                    tuple, size, opcode template with explicit field placement,
                    CPU level
    isa/operands.*  Operand classifier (§3.2): candidate classes per operand
    isa/select.*    Generic-mnemonic resolution and shortest-legal form
                    selection (Keil rules)
    isa/encode.*    Template → bytes
    assemble.*      Pass driver:
                      P1 – layout with optimistic (short) branches;
                      Pn – re-layout, growing forms until a fixpoint
                           (monotonic, so it always terminates);
                      final – emit bytes and diagnostics
    locate.*        Mini-locator for relocatable sections (base /
                    per-section addresses)
    output/         Raw binary (base, fill byte, range clip), Intel HEX,
                    S-records, Keil-style listing (LOC OBJ LINE SOURCE), JSON map
    diag.*          Diagnostics carrying Keil-compatible codes where practical
                    (A6, A62, A70, A74, A77, A78, A108, A110, A111, A132, …)
    capi.cpp        C API implementation
  tools/isagen.cpp  Built by CMake. Turns c166.tbl into a constexpr C++ table
                    and an isa.json for third-party tools. No Python needed to
                    build.
  cli/main.cpp      openc166 src.a66 -o out.bin [--base 0xC000] [--entry SYM|ADDR]
                    [--fill 0xFF] [-I dir] [-D SYM=val] [--format bin|hex|srec]
                    [--listing] [--map]
                    Also accepts A166-style controls:
                    openc166 src.a66 SET(X) INCDIR(dir)
  python/           nanobind module + pyproject.toml (scikit-build-core).
                    openc166.assemble(src|path, *, include_paths, defines,
                    externs, base, sections, fill) -> Result(image: bytes,
                    load_addr, entry, symbols, sections, listing, diagnostics)
  tests/            C++ unit tests (doctest), plus the pytest differential
                    harness (§6) driving the CLI and Python module
```

### 5.3 C API sketch

(Superseded by [include/openc166/openc166.h](../include/openc166/openc166.h).)

```c
oc166_ctx *oc166_create(void);
void       oc166_destroy(oc166_ctx *);
int  oc166_set_option(oc166_ctx *, const char *name, const char *value);
int  oc166_add_include_dir(oc166_ctx *, const char *dir);
int  oc166_define(oc166_ctx *, const char *sym, int64_t value);
int  oc166_set_extern(oc166_ctx *, const char *sym, uint32_t addr);
int  oc166_assemble_file(oc166_ctx *, const char *path);
int  oc166_assemble_string(oc166_ctx *, const char *src, const char *name);
const uint8_t *oc166_image(const oc166_ctx *, size_t *len,
                           uint32_t *load_addr, uint32_t *entry);
size_t oc166_symbol_count(const oc166_ctx *);   /* + oc166_symbol_at() */
size_t oc166_diag_count(const oc166_ctx *);     /* + oc166_diag_at()   */
const char *oc166_listing(const oc166_ctx *);
uint32_t oc166_api_version(void);
```

### 5.4 Distribution

- **Static CLI binaries** are attached to GitHub releases for:
  - Linux x86_64/aarch64 (musl, fully static);
  - macOS universal2;
  - Windows x64/arm64 (MSVC `/MT`).
- **Python wheels** are built with cibuildwheel. They are `abi3` wheels for CPython ≥ 3.12 (OpenGK's minimum), with the same platforms as above.
- **Library** consumers can use `find_package(openc166)`/`pkg-config`, or `add_subdirectory`.
- **Safety:** CI runs the tests under ASan/UBSan, and a libFuzzer target exercises the parser and expression evaluator.

### Design points

- **Single source of truth.** `isa/c166.tbl` drives the encoder, the generated test sweeps, `isa.json` and, optionally, a disassembler for round-trip self-checks. Other projects can reuse the table.
- **Load vs execution address.** The load address comes from `SECTION … AT` or `--base`/`sections=`. The entry point is a symbol or address, reported separately in the API result. A raw image covers `[min_addr, max_addr]` with configurable fill, and overlaps are errors.
- **Externals.** ECU payloads often call ROM routines. `oc166_set_extern()`, Python `externs={"FlashWrite": 0x8A2C}`, or CLI `-D` resolves `EXTERN` symbols without a linker.
- **Register definitions.** Ship clean-room register include files generated from datasheet tables, not copies of Keil's `REG*.INC`. Users can still `$INCLUDE` their own Keil files, because the parser supports `DEFR`/`BIT`/`EQU`.

---

## 6. Differential test strategy

All test inputs are written independently, or generated from our own
specification; none are copied from Keil or binutils.

| Layer | Inputs | Oracle(s) | Where it runs |
|---|---|---|---|
| **A. ISA sweep** | Generated from `isa/c166.tbl`: every form × boundary operands (all GPRs; data3/4/8/16 edges; bitoff 00/7F/80/EF/F0–FF; rel −128/+127; all cc, trap, irang and pag values) | **ASL** (open source, built in CI) **and** Keil A166 `.LST`; Ghidra SLEIGH decode round-trip | Public CI on all three OSes (ASL); Keil opt-in |
| **B. Keil semantics** | Hand-written probes for generic mnemonics, typing errors, ASSUME/DPP, operators, relaxation, sections/ORG/EVEN, conditionals and includes | Keil A166 `.LST` bytes per line, **plus** diagnostic class (error, warning, none) | Opt-in Keil job (Linux + Wine, or native Windows) |
| **C. Real-world corpus** | The acceptance tests in §6.1, plus the user's own future payload sources | Golden binaries | MiniMon sources and HEX files: local only (© Infineon). OpenGK MTOS payloads: public |
| **D. Differential fuzzing** | Random valid programs drawn from the grammar | Keil, ASL (instruction subset) | Nightly or manual |

### Harness notes

Prototyped in the session as `pilot.py`.

- **Batching.** Many cases are batched into one source file. Each case's bytes are mapped back to its line through the LST `LINE` column, so A166 runs about once per batch.
- **Avoiding the linker limit.** Use `SECTION CODE AT …` so that the LST holds final bytes. This sidesteps L166's 4 KB eval limit, since the A166 eval is not size-limited. Externals show up as `????`.
- **Locating the tools.** Point to a Keil install with `KEIL_C166=/path`, run under an isolated `WINEPREFIX`, and never redistribute binaries or Keil-generated artifacts.
- **ASL as the public baseline.** ASL syntax differences (`cc_` stripping, `[Rw+#d]` → `[Rw+d]`) are handled by a small translator in the test harness only.

Pilot result: **142/142 hardware-instruction forms byte-identical (Keil vs ASL)**.
The only divergences found were syntactic or semantic differences, which are
exactly the Keil behaviours the new tool must follow (§4).

### 6.1 Acceptance tests (gkflasher payloads)

An acceptance test is a real program whose correct output we already have byte for byte. openc166 passes it only if it reproduces that output exactly.

Investigating gkflasher showed that all of its ECU-side code comes from **Infineon MiniMon**, which was built with the TASKING toolchain:

- `bsl.py` speaks the MiniMon protocol (`A_ACK1`=0xAA, `C_CALL_MEMTOOL_DRIVER`=0xCF, …).
- `assets/simk4x_bootstrap.bin` (32 B) is byte-identical to MiniMon `MCPROG/LOADK.HEX`. That file is the K-line build of `LOAD.A66` (`@SET(KLINE,1)` selects `JNB S0RIR`, encoded `9AB7FE70`).
- `assets/simk4x_kernel.bin` (394 B) is byte-identical to `MCPROG/MINIMONK.HEX`, the K-line build of `MINIMON.A66`.
- `simk4x_driver_{v6,i4}_a29fx00bx.bin` are modified derivatives of the MiniMon `Driver/A29F200b` and `A29F400b` drivers. Their modified source has not been found.

| ID | Input | Expected output | Exercises |
|----|-------|-----------------|-----------|
| AT1 | `LOAD.A66` with `KLINE=1`, placed at 0FA40H | `simk4x_bootstrap.bin` | TASKING subset, `@IF`, placement |
| AT2 | `MINIMON.A66` with `KLINE=1`, placed at 0FA60H, built together with AT1 | `simk4x_kernel.bin` | Cross-file `EXTERN`/`GLOBAL`, `PROC FAR`, `RETV` |
| AT3 | MiniMon `Driver/A29F200b` and `A29F400b` sources | Their shipped `.hex` | Real flash-driver code |
| AT4 | gkflasher MTOS payloads rewritten as `.a66` (entry at 0C014H, ends with `RETS`) | Bytes from `mtos_asm.py` | Keil syntax, raw binary at an explicit base address |
| AT5 | Keil probe suite (§6 layer A/B) and Keil `START167.A66`-class files | Keil A166 output | Keil compatibility |

AT1–AT3 use © Infineon sources and stay in a local, uncommitted corpus. AT4 is public (gkflasher, GPL-3.0); only the expected bytes and independently written `.a66` sources go into the openc166 repository.

---

## 7. Licensing and compatibility concerns

| Item | Concern | Mitigation |
|---|---|---|
| **Keil EULA** (Arm, eval mode) | <ul><li>Reverse engineering is prohibited (except where law permits).</li><li>**Benchmarking clause:** results "indicative of performance, efficacy, reliability or quality" must be kept confidential.</li><li>Academic/eval versions may be restricted to non-commercial use.</li></ul> | <ul><li>Black-box output comparison only; never disassemble A166.EXE.</li><li>Do not publish Keil-generated listings or comparison reports. Public expected bytes come from the ISM, ASL or our own review.</li><li>Keil tests run only on the developer's own licensed install.</li><li>**Get a legal opinion if CI publishing is planned.**</li></ul> |
| Keil manual, `REG*.INC` (© Keil 1993–99) | Copyrighted text and files | Implement behaviour and syntax (functional compatibility). Write our own docs. Do not ship Keil `.INC` files; generate our own from datasheet register tables. |
| Infineon ISM / datasheets | Copyrighted documents | Encode facts (opcodes, fields) in our own table format; do not reproduce the text or tables verbatim. |
| ASL | GPL-2.0-only OR GPL-3.0-only | Use only as an **external test oracle**, which places no license obligation on our code. Do not copy code. If code were reused, openc166 would have to be GPL-2/3-only. |
| binutils xc16x | GPL-3.0+, removed upstream | Reference only. Do not copy the test `.s` files. |
| vasm c16x | Non-commercial license | Do not use or read. |
| Ghidra c166 module | MIT | Free to use as a test dependency or for table cross-checks, with attribution. |
| MiniMon sources/hex | © Infineon 1998–2000, no license | Local test corpus only; never commit. |
| OpenGK / gkflasher | GPL-3.0 | **Decided: openc166 is MIT**, which is compatible with OpenGK and lets proprietary tuning and IDE tools embed it. |

---

## 8. Build vs adapt — recommendation

**Build new (clean room).** Rationale:

1. **Keil compatibility is the product**, and none of the candidates has it. Retrofitting typed symbols, `SECTION`/`PROC`/`ASSUME` semantics, generic mnemonics, `$`-controls and Keil's operand typing into ASL means rewriting its front end and much of `code166.c` inside a 300-CPU monolith. That would be a long-lived fork with GPL-2/3-only constraints.
2. **The ISA core is small.** An instruction table is about 120 forms and would take days to write. Two independent oracles (ASL and Keil) are already shown to agree.
3. **Integration for every consumer.** The C ABI, static CLI and Python wheels let OpenGK and every other tool get `bytes` plus a symbol map in-process (§5.1).
4. **Reuse what is safe:**
   - ASL as a binary oracle;
   - Ghidra SLEIGH (MIT) as a decode cross-check;
   - the ISM as the normative spec;
   - Keil as the behavioural oracle (local, opt-in).

---

## 9. Implementation roadmap

Estimates are for one experienced developer. The exit criteria are measurable.

| Milestone | Content | Effort | Exit criteria |
|---|---|---|---|
| **M0 Bootstrap** | CMake ≥ 3.20 project (library, CLI and test targets), CI matrix (Linux/Windows/macOS × GCC/Clang/MSVC, with ASan/UBSan), ASL oracle build in CI, pytest differential harness, opt-in Keil/Wine job | 1 wk | CI green on all three OSes; harness reproduces the 142-form pilot |
| **M1 ISA core** | `isa/c166.tbl` (all 84 mnemonics), `isagen`, operand classifier, encoder, numeric expressions only | 2–2.5 wk | Generated sweep (thousands of cases) 100% identical to ASL and Keil; SLEIGH round-trip clean |
| **M2 Keil front end** | Lexer and parser, numbers and strings, full operator precedence, typed symbol table, `EQU`/`SET`/`LIT`/`BIT`/`DEFR`/`DEFB`/`DEFA`/`DBIT`, `cc_`, generic mnemonics, `PROC`/`ENDP` and `RET` resolution, label typing | 2 wk | Layer-B probes for typing and generics match Keil, including diagnostics |
| **M3 Sections and addressing** | `SECTION`/`ENDS` (AT and relocatable), `ORG`/`EVEN`/`DS*`/`DB`/`DW`/`DD`, `$`, `ASSUME`, `DPPx:`, `PAG`/`POF`/`SEG`/`SOF`/`BOF`, `SEGMENTED`/`NONSEGMENTED`, EXTx windows (A110/A111), relaxation fixpoint, mini-locator | 2 wk | Relaxation and DPP probe suites match Keil byte for byte |
| **M4 Preprocessor and includes** | `$INCLUDE` with search path, `$IF`/`$SET`/`$RESET`, statement `IF`, `$CASE`, `MOD*` gating (A108), clean-room register include files, multi-file builds | 1 wk | Keil board `START167.A66`-class sources (without MPL) assemble identically |
| **M4b TASKING subset** | Tier 1b (§4.1): `RETN`/`RETV`, `EXTEND`, `STDNAMES`, `@SET`/`@IF`/`@ELSE`/`@ENDI`, `label: NEAR`, `.loc`-style placement | 1–1.5 wk | Acceptance tests AT1–AT3 pass (§6.1) |
| **M5 Outputs, CLI and C API** | Raw binary (base, fill, entry, per-section or per-range split), Intel HEX, S-records, Keil-style listing, JSON map; GNU-style and A166-style CLI; C API plus C++ wrapper | 1.5 wk | C API covered by tests; CLI output identical to the library API |
| **M6 Python wheels and OpenGK integration** | nanobind module, scikit-build-core packaging, cibuildwheel (abi3, all platforms); port MTOS payloads to `.a66`; replace `mtos_asm.py` call sites | 1.5 wk | Wheels install and pass tests on all platforms; MTOS payloads byte-identical to `mtos_asm.py` output (AT4); gkflasher tests pass |
| **M7 Hardening and release** | libFuzzer plus differential fuzzing, error-recovery polish, docs, static release binaries, PyPI 0.1 | 1.5–2 wk | No fuzz crashes or divergences over N runs; docs complete |
| **Tier 2** (optional) | Full M166 macro language, `MACRO`/`REPT`/`IRP`, `PECDEF`, WASM build for a browser playground | +3–5 wk | The remaining MiniMon drivers reproduce their shipped `.hex` |

**Tier 1 total: about 13–15 person-weeks.**

---

## 10. Risks, open questions and decisions needed

**Decisions (all resolved for Phase 2)**

1. ~~Implementation language~~ **Decided:** C++20 core + C API + CLI + Python wheels (§5).
2. ~~Project license~~ **Decided:** MIT.
3. ~~Toolchain floor~~ **Done:** `cmake_minimum_required(VERSION 3.20)` (verified with CMake 3.28). Minimum compilers for C++20: GCC ≥ 11, Clang ≥ 14, MSVC 2022.
4. ~~Dialect priority~~ **Decided:** Keil first, then the TASKING subset (Tier 1b) within the 0.1 release, because gkflasher's existing payloads are TASKING code.
5. ~~Acceptance sources~~ **Decided:** see §6.1.

**Behaviours still to confirm with Keil (cheap, harness exists)**

- Sizing of forward-referenced immediates: does `MOV R1,#later_equ` pick the data4 or the data16 form?
- Implicit word alignment of `DW`/`DSW`, and the fill value Keil uses for alignment gaps.
- `JMPS`/`CALLS` selection across segments in SEGMENTED mode, and `CALL` relaxation inside `PROC FAR`.
- The exact diagnostic set for DPP misuse (A77) and type errors.

**Risks**

| Risk | Impact | Mitigation |
|---|---|---|
| Undocumented Keil corner cases | Byte mismatches | Layers B and D; fix-forward against the oracle |
| Keil EULA limits on testing or publication | Reduced public CI | ASL as the public oracle; Keil local and opt-in |
| Real payloads use TASKING features beyond Tier 1b, or MPL macros | Scope creep | Tiered scope; acceptance tests (§6.1) fix the required subset |
| Variant differences (C166/C167/ST10/XC16x) | Wrong SFR or ISA assumptions | CPU-level gating in the spec; per-variant register files |
| C++ memory-safety bugs on untrusted input | Crashes in host tools | ASan/UBSan in CI, libFuzzer, no raw ownership in the core |
| Wheel and binary build matrix churn | Release friction | cibuildwheel plus abi3 (one wheel per platform); static CLI builds in CI |
