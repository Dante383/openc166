# The TASKING dialect

Keil and TASKING both made a C166 assembler called "a166", and both read
the same instructions. They differ in their controls, some directives, the
names they know, and their macro preprocessors. openc166 reads Keil source
by default. With `--dialect tasking` (Python: `dialect="tasking"`, C:
`oc166_options_set_dialect(opts, "tasking")`) it reads TASKING source
instead.

The target is the subset that real TASKING programs use, checked against
Infineon's MiniMon monitor and flash drivers: openc166 rebuilds all 25 of
their HEX files byte for byte (see [the acceptance test](#acceptance-test)).
The reference is the TASKING *Cross-Assembler, Linker/Locator, Utilities*
manual (v8.8). There is no TASKING assembler to compare with line by line,
as there is for Keil.

```sh
openc166 --dialect tasking -c "CASE EXTEND STDNAMES(reg167.def)" \
         --section CODE1=0FA40H -o payload.bin payload.asm
```

## What happens to a TASKING source

TASKING runs two programs: the m166 macro preprocessor, then the assembler.
openc166 does both in one go, line by line. Each line goes through
openc166's m166 first, and the assembler reads what comes out.

The CPU is always the C167: TASKING has no C166-only mode. `EXTEND` (the
C167 instructions) is accepted, and `NOMOD166` is ignored.

## m166

m166 works on characters, not statements, so its calls work anywhere on a
line, even in the middle of an operand. openc166 supports:

| m166 | Meaning |
|---|---|
| `@SET(name, value)` | sets a macro variable to a number (an expression) |
| `@name` | the value of a macro variable, or the text of a `DEFINE` control |
| `@EVAL(expr)` | the value of an expression, as m166 writes it: `@EVAL(12)` gives `0Ch` |
| `@IF(expr)` ... `@ELSE` ... `@ENDI` | keeps one branch of text; may be nested |
| `@DEFINED(name)` | 1 if the macro exists, else 0 |
| `@"text"` | a comment; the text disappears |
| `@"text` | a comment that takes the end of the line with it: the next line joins this one |

Expressions use m166's operators, in words or in C style: `+ - * /`,
`MOD` (`%`), `SHL` (`<<`), `SHR` (`>>`), `AND` (`&`), `OR` (`|`), `XOR`
(`^`), `~`, `HIGH`, `LOW`, the comparisons `EQ NE LT LE GT GE` (`== != <
<= > >=`) and their unsigned forms `ULT ULE UGT UGE`, the logical `NOT`
(`!`), `&&` and `||`, and parentheses. Numbers are 32 bits wide.

Error OC29 marks an m166 function that openc166 does not have yet, such as
`@DEFINE` (macros with parameters), `@WHILE`, `@REPEAT`, `@MATCH` or
`@LEN`. Error OC30 marks a mistake in the m166 text itself: an undefined
macro, an `@ELSE` or `@ENDI` without `@IF`, or an `@IF` that is never
closed.

## Controls

Controls come from `$` lines in the source, as in Keil, or from the command
line (`-c TEXT`, which may be repeated; Python: `controls=[...]`; C:
`oc166_options_add_control`). Command-line controls are read first, as if
they stood at the top of the source. Errors in them are reported against
`<command line>`.

TASKING's abbreviations work (`SN` for `STDNAMES`, `EX` for `EXTEND`, `CA`
for `CASE`, `DEF` for `DEFINE`, ...).

| Control | Effect in openc166 |
|---|---|
| `CASE`, `NOCASE` | as in Keil: whether names are case sensitive |
| `SEGMENTED`, `NONSEGMENTED` | as in Keil; see [Segmented mode](compatibility.md#segmented-mode-and-dpps) |
| `INCLUDE(file)`, `GEN`, `NOGEN`, `LIST`, `NOLIST`, `SAVE`, `RESTORE`, `TABS` | as in Keil |
| `STDNAMES(file)` | reads a register file: see [Register names](#register-names) |
| `DEFINE(name[, text])` | defines an m166 macro; without text it is `1` |
| `EXTEND` | the C167 instructions (always on) |
| `EXTEND1`, `EXTEND2`, `EXTEND22`, `EXTMAC` | warning OC2: the extensions of these newer cores are not supported, the C167 set is used |
| `NOOPTIMIZE` | warning OC31: generic `JMP` and `CALL` still pick the shortest form |
| listing, object-file and check controls (`DEBUG`, `SYMB`, `NOMOD166`, `CHECKC166SV1DIV`, `TITLE`, ...) | accepted and ignored |
| anything else | error A16 |

As in Keil, some controls must come before the first statement (`CASE`,
`SEGMENTED`, `STDNAMES`, `DEFINE`, `EXTEND`, ...). The lines of a
`STDNAMES` file, and the files it includes, do not count as statements.

## Register names

TASKING knows only a few names by itself: the registers `R0`–`R15` and
`RL0`–`RH7`, the condition codes, the core SFRs (`DPP0`–`DPP3`, `CSP`,
`MDH`, `MDL`, `MDC`, `CP`, `SP`, `STKOV`, `STKUN`, `PSW`, `ZEROS`, `ONES`)
and the flag bits of `PSW` (`N`, `C`, `V`, `Z`, `E`, `MULIP`, `USR0`,
`HLDEN`, `IEN`). The other SFRs (`S0CON`, `P2`, `ADCON`, ...) come from a
register file named by `STDNAMES(file)`, such as `reg167.def`, which came
with TASKING.

openc166 reads the `STDNAMES` file like an include file. If it cannot find
it, it uses its own built-in `REG167.INC` instead and says so (warning
OC32). The same file named twice, on the command line and in the source, is
read once.

Register files define names with these directives:

| Directive | Meaning |
|---|---|
| `name DEFR addr` | an SFR with a short (8-bit) address, as in Keil |
| `name DEFA addr` | a system address in `0C000H..0FFFEH` (even, not the PEC pointers `0FCE0H..0FCFEH`); Keil's `DEFA` allows only `0FA00H..0FDFEH` |
| `name DEFX addr` | any address; the source must give the `DPP` |
| `name DEFB bit` | a bit, as in Keil |
| `name DEFBF ...`, `DEFVAL ...` | bit fields and values for TASKING's debugger: ignored |

After the address, TASKING allows more fields (access, reset value,
comment: `ADCON DEFR 0FFA0H, RW, 0, 0000H, "A/D control"`). openc166 reads
the access (`R`, `RW` or `W`) and ignores the rest.

## Syntax that differs from Keil

- **Returns.** `RETN` is always the near return (`RET`); `RETV` produces no
  code. `RET` becomes `RETS` in a `FAR` procedure, as in Keil.
- **Procedures.** `PROC` without a type is `NEAR` in non-segmented mode
  and `FAR` in segmented mode. A procedure without `ENDP` ends at the
  `ENDS` of its section (Keil reports A106).
- **Label types.** A label in a `FAR` procedure is `FAR` (in Keil it is
  `NEAR`). `label: NEAR` or `label: FAR` on a line of its own gives the
  label that type.
- **Jumps.** A generic `JMP` to a `FAR` label in the same section becomes
  `JMPR` or `JMPA`, not `JMPS`: the jump does not leave the code segment.
  `CALL` to a `FAR` label is still `CALLS`, because the procedure returns
  with `RETS`. There is no warning A84 for a near jump to a `FAR` label.
- **Register banks.** `REGDEF` alone means `R0-R15`. Ranges may be
  followed by `PRIVATE` or `COMMON = name`, and `COMREG` names a common
  range. Only the linker cares about these, so openc166 checks their
  syntax and otherwise ignores them. A `REGDEF` or `REGBANK` without a name
  reserves no memory in openc166.
- **Line information.** Lines starting with `#` (such as `# 12 "main.c"`,
  which TASKING's C compiler writes) are ignored.

## Placing sections

TASKING's locator places sections according to a `.loc` file or the
`ADDRESSES(SECTIONS(...))` control. openc166 has no locator file; give the
same addresses with `--section NAME=ADDR` (Python: `sections={...}`).
Sections without an address are placed as Keil's L166 would place them.
`SECTION ... AT` works as in Keil.

## Acceptance test

[tests/acceptance/minimon.py](../tests/acceptance/minimon.py) rebuilds
Infineon's MiniMon monitor (four variants) and its flash drivers from their
sources and compares every byte with the HEX files that TASKING made of
them: 25 builds, all identical. MiniMon is not part of openc166 (it is
copyrighted by Infineon and Siemens), so the test runs only when the
environment variable `MINIMON_DIR` names a MiniMon directory; `ctest`
reports it as skipped otherwise.

Two MiniMon files need a small fix-up, which the script makes in a
temporary copy:

- The source of the A29F010 driver is newer than its HEX file. The script
  removes the one instruction that was added later.
- The rstcon driver uses `RSTCON`, a register of the C161CS and C167CS that
  is not in `REG167.INC`. The script supplies a small `STDNAMES` file that
  defines it.

## Not supported

- m166 macros with parameters (`@DEFINE`), loops and string functions
  (error OC29)
- the extended instruction sets of `EXTEND1`, `EXTEND2`, `EXTEND22` and
  `EXTMAC` (XC16x, Super10)
- TASKING's locator (`.loc` files), object files and libraries
