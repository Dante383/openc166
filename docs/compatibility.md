# Compatibility with Keil A166

openc166 targets the syntax and output of Keil A166 (checked against V5.38).

## The rule

1. **Source that Keil assembles cleanly must give byte-identical code** in
   openc166, at the same addresses, with the same symbol values and the
   same warnings.
2. **Source that Keil rejects is rejected too.** The error code may differ
   when Keil and openc166 trip over different parts of a broken line.
3. **Where Keil silently produces wrong code, openc166 reports an error.**
   A payload that runs on a real ECU should never depend on these cases.
   They are listed below, and the test corpus marks each one with
   `; DIVERGES:`.

The differential test in `tests/diff/` checks rules 1 and 2 line by line.
With `--image` it also links Keil's output with L166 and OH166 and compares
the complete memory image.

## Supported

- **CPUs:** C166 (the default) and C167. Select C167 with `$MOD167` or
  `--cpu c167`. In C166 mode, the C167-only instructions (`ATOMIC`, `EXTR`,
  `EXTP`, `EXTPR`, `EXTS`, `EXTSR`) fail with A108, as in Keil.
- **Instructions:** all C166/C167 mnemonics and addressing modes. Keil's
  automatic form selection is reproduced, including:
  - short `#data3`/`#data4` forms;
  - `RET` becoming `RETS`/`RETI` inside `FAR`/`INTERRUPT` procedures;
  - generic `JMP` and `CALL`, see [Jumps and calls](#jumps-and-calls);
  - bit, bit-field and extended-SFR addressing.
- **Expressions:** `+ - * / MOD SHL SHR AND OR XOR NOT`, comparisons,
  `HIGH LOW SOF SEG PAG POF`, `$`, character constants, and bit selection
  (`base.n`), and the `SHORT` and `type PTR` operators. See
  [Expression details](#expression-details).
- **Directives:** `SECTION ... [AT addr]` / `ENDS`, `PROC [NEAR|FAR|INTERRUPT|TASK]`
  / `ENDP`, `LABEL`, `EQU`, `SET`, `BIT`, `DB`, `DW`, `DD`, `DS`, `DSB`, `DSW`,
  `DSD`, `ORG`, `EVEN` and `END`.
- **Register definitions:** `DEFR`, `DEFA` and `DEFB`, the directives that
  register files such as `reg167.inc` use. See
  [Register definitions](#register-definitions).
- **Include files:** `$INCLUDE(file)` (short form `$IC`) and
  `$INCDIR(dir;dir...)` (`$ID`). See [Include files](#include-files).
- **Accepted and ignored:** these only matter to a linker or debugger.
  - Directives: `NAME`, `PUBLIC`, `GLOBAL`, `ASSUME`, `REGDEF`, `REGBANK`,
    `SSKDEF`, `PECDEF`, `TYPEDEC`.
  - Listing and object-file controls: `$NOLIST`, `$TITLE(...)`, `$DEBUG`,
    `$SEGMENTED`, and similar.
- **Controls:** `$MOD167`, `$MOD166`/`$NOMOD166`, `$CASE`/`$NOCASE`.
  `$MODV2` falls back to the C167 set, with warning OC2. Some controls must
  come before the first statement; see [Controls](#controls).

## Not supported yet

These produce error OC3 (or OC4 for externals), so a missing feature is never
silently skipped:

- `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF` and `$SET`/`$RESET`
- `MACRO` and the other macro facilities
- `EXTERN`/`EXTRN` (a raw image has no linker to resolve them)
- `DBIT`, `DGROUP`, `CGROUP`, `COMREG`, and BIT sections
- the XC16x/Super10 extended instruction set

`LIT` belongs to Keil's AX51 assembler. A166 rejects it with A6, and so does
openc166.

## Register definitions

- **`name DEFR addr [, R|RW|W]`** names an SFR. It works exactly like a
  built-in SFR such as `PSW`, including the short register forms. The
  address must be even and in `0FE00H..0FFDEH`, or in `0F000H..0F1DEH` (the
  extended SFRs, C167 only). Otherwise the error is A101. A name that is
  already an SFR (`DEFR PSW`) is also A101, but `PSW+2` works.
- **`name DEFA addr [, ...]`** names a word of internal RAM, an even address
  in `0FA00H..0FDFEH`. It acts like a `WORD` variable, so `JMP` to it warns
  with A74. A bad address gives A102.
- **`name DEFB word.bit [, ...]`** names a bit. It must be exactly
  `word.bit`; parentheses are fine. `PSW.3+1` or a bit name such as `C`
  gives A103, and a GPR bit (`R1.2`) gives A70.

The access attribute only matters to a debugger and is ignored. As in Keil:

- when a definition fails, its name stays undefined;
- a forward reference gives the error but keeps a `DEFA` or `DEFB` name
  usable;
- a `DEFR` name defined by a forward reference cannot be used (A42).

In C166 mode there are no extended SFRs, so a bit address in that area
fails with A68, as in Keil.

## Include files

`$INCLUDE(file)` reads another file in place of the control line, like
`#include` in C. Included files may include further files, up to 32 levels
deep, and the same file may be included more than once. Spaces inside the
parentheses are ignored; quotes are not removed, so `$INCLUDE("x.inc")`
looks for a file whose name starts with a quote, as in Keil.

A file is searched for in this order, and the first match wins:

1. the current directory;
2. the directory of the **main** source file (not of the file that contains
   the `$INCLUDE`);
3. directories given with `-I` on the command line, in order;
4. directories given with `$INCDIR(dir;dir...)`, in order.

Names are matched without regard to upper and lower case, on every
operating system, because Keil sources written on Windows often mix them.
A path may use `\` or `/`.

`$INCDIR` adds directories for all `$INCLUDE` lines after it. Keil looks
for a relative `$INCDIR` directory only from the current directory.
openc166 also tries it relative to the main source file, so that
`openc166 project/main.a66` works from any directory.

**Listing.** As in Keil, one line counter runs through all files, and
lines from an included file are marked `+1`, `+2`, ... by nesting level. A
message about a line in an included file names that file and the line
number inside it.

**A last line without a line break.** When an included file does not end
with a line break, Keil glues its last line to the start of the next line
it reads. A comment on that last line then hides the next statement.
openc166 does the same, and warns with OC8 when both parts contain text.

**Errors.** A missing file (OC5) or nesting deeper than 32 levels (OC6)
stops the assembly; Keil stops too. `$INCLUDE` must be the first control on
its line: `$INCLUDE(x) MOD167` is A171 and includes nothing, and
`$NOLIST INCLUDE(x)` is ignored with warning OC7.

## Controls

Keil has two kinds of `$` controls:

- **Primary controls** set up the whole assembly and must come before the
  first statement. Comments, blank lines and general controls may come
  first, as may an included file that has only those. A primary control
  after that is error A8, and the **whole line** is ignored: a late
  `$MOD167` does not switch to the C167, so a later `EXTR` fails with A108.
  The one exception, as in Keil, is `$INCDIR`, which still takes effect.
- **General controls** may appear anywhere.

Primary controls that openc166 uses: `MOD167`/`M167`, `MOD166`/`M166`,
`NOMOD166`/`NOM166`, `MODV2`, `CASE`, `NOCASE`, `INCDIR`/`ID`. Those it
ignores: `PAGELENGTH`/`PL`, `PAGEWIDTH`/`PW`, `SYMBOLS`/`SB`,
`NOSYMBOLS`/`NOSB`, `XREF`/`XR`, `NOXREF`/`NOXR`, `DEBUG`/`DB`,
`NODEBUG`/`NODB`, `PRINT`/`PR`, `NOPRINT`/`NOPR`, `OBJECT`/`OJ`,
`NOOBJECT`/`NOOJ`, `ERRORPRINT`/`EP`, `NOERRORPRINT`/`NOEP`, `PAGING`,
`NOPAGING`, `TABS`, `DATE`, `LINES`, `NOLINES`, `TYPE`, `NOTYPE`, `AMAKE`,
`NOAMAKE`, `SEGMENTED`, `NONSEGMENTED`, `USEDEXTONLY`, `EXTMAC`, `MACRO`,
`NOMACRO`, `MPL`, `NOMPL`, `EXPDECNUM`, `ABSOLUTE`, `NOABSOLUTE`.

General controls, all ignored: `LIST`, `NOLIST`, `TITLE`/`TT`, `EJECT`/`EJ`,
`GEN`, `NOGEN`, `GENONLY`/`GO`, `COND`, `NOCOND`, `SYMLIST`, `NOSYMLIST`,
`SAVE`, `RESTORE`, `REGUSE`.

These lists come from testing A166 V5.38. The manual calls `TITLE` and
`NOCOND` primary, but A166 accepts them anywhere. An unknown control is
A16.

## openc166's own messages

Messages that Keil does not have use codes starting with `OC`:

| Code | Kind | Meaning |
|---|---|---|
| OC1 | warning | two pieces of code or data overlap in the image |
| OC2 | warning | `$MODV2`: the XC16x/Super10 extensions are not supported |
| OC3 | error | a control or directive that openc166 does not support yet |
| OC4 | error | external symbols, which need a linker |
| OC5 | error | an include file cannot be opened; the assembly stops |
| OC6 | error | include files nested too deeply; the assembly stops |
| OC7 | warning | `$INCLUDE` that is not the first control on its line, ignored |
| OC8 | warning | the last line of an include file was joined to the next line |

## Data alignment

Keil moves word data to an even address and double-word data to a multiple
of 4. openc166 does the same:

| Directive | Alignment |
|---|---|
| `DB`, `DS`, `DSB` | none |
| `DW`, `DSW` | 2 |
| `DD`, `DSD` | 4 |

The skipped bytes are not written; in the binary they get the `--fill`
value. A label on the same line (`T1: DW 1`) gets the aligned address. A
label alone on its line keeps the address it had.

Instructions and `PROC` are never moved. Keil warns with A48 ("insufficient
alignment") when one of these is at an odd address, and openc166 does too:

- a label alone on its line, or in front of an instruction, inside a `CODE`
  section
- `LABEL WORD`, `LABEL NEAR` or `LABEL FAR` at an odd address
- `LABEL DWORD` at an address that is not a multiple of 4
- `PROC`

Keil checks this in every pass, so a label that only lines up after
forward jumps were shortened still gets the warning. openc166 does the same.

## Jumps and calls

Labels have a type, shown in the symbol table of the listing:

| Label | Type |
|---|---|
| `name:` in a `CODE` section, `LABEL NEAR`, `PROC NEAR` | NEAR |
| `PROC FAR`, `LABEL FAR` | FAR |
| `name:` outside `CODE` sections, `DB`/`DS`/`DSB`, `LABEL BYTE` | BYTE |
| `DW`/`DSW`, `LABEL WORD` | WORD |
| `DD`/`DSD`, `LABEL DWORD` | DWORD |

A colon label in front of data (`T: DW 1`) takes the type of the data. A
colon label inside a `FAR` procedure is still NEAR. `FAR PTR x`,
`NEAR PTR x`, `WORD PTR x` and so on change the type of an address.
Arithmetic removes the type (`label + 2` is untyped), except plus or minus
zero.

`JMP` and `CALL` pick an instruction the way Keil does:

1. `[Rw]` gives `JMPI`/`CALLI`.
2. A FAR target without a condition gives `JMPS`/`CALLS`. With a
   condition (even `cc_UC`) it gives `JMPA`/`CALLA` and warning A84.
3. `SHORT target` forces `JMPR`/`CALLR`.
4. Otherwise the relative form is used when the target is in range
   (−256 to +254 bytes), and `JMPA`/`CALLA` when it is not. `CALL` only
   uses `CALLR` without a condition or with `cc_UC`.

Forward jumps start long and are shortened in later passes, using the
target address of the previous pass. That is why a forward jump that would
reach exactly +254 bytes once shortened stays long, as in Keil.

Warnings: A74 for a jump or call to a data label (BYTE, WORD or DWORD), A84
for a near jump or call to a FAR label, A134 for an odd absolute target.

## Expression details

Plain numbers are signed 64-bit values. Labels work differently in Keil, and
openc166 copies that, because the result can change the code.

**Short immediates.** Some instructions have a short form for small values
(`MOV R1, #2` is 2 bytes instead of 4). Keil uses the long form whenever the
value might need a linker fixup:

| Immediate | Form |
|---|---|
| a label or `$`, alone or plus zero (`#L1`, `#L1+0`) | long |
| `SEG x`, alone or plus zero | long |
| a label difference (`#L1-L0`) | short if the value fits |
| label ± a non-zero number or difference, in a section with `AT` | short if the value fits |
| label ± anything, in a section without `AT` | long |
| anything built with `*`, `/`, `AND`, `HIGH`, `LOW` and similar | short if the value fits |

**Unsigned results.** A label difference (`L1 - L0`), and a label ± a
non-zero number in a section with `AT`, is an **unsigned** number calculated
modulo 2^61:

- `DW L0 - L1` with `L1` two bytes after `L0` gives `0FFFEH` and warning A62.
- `(L0 - L1) GT 0` is true.
- `DW L1 - 0C010H` with `L1` at `0C002H` gives warning A62.

`a + b` and `a - b` are unsigned when the left side is unsigned, and signed
when it is a plain number: if `L1 - L0` is 2, then `1 - (L1 - L0)` is −1,
with no warning. `*`, `/`, `MOD`, shifts, `AND`, `OR` and `XOR` are unsigned
if either side is. Unary `-`, `NOT`, `HIGH`, `LOW` and similar give a signed
number.

**Registers as numbers.** In an immediate (`#PSW`) or a displacement
(`[R1+#PSW]`), Keil reads a register name as its address: an SFR as
itself, a GPR as the SFR-area word that matches its short register number
(`#R1` is `0FFE2H`, `#RL1` is `0FFE4H`). openc166 does the same. Elsewhere,
for example in `DW R1`, a GPR is an error (A70).

## Error recovery

As in Keil, a bad item in a `DB`/`DW`/`DD` list does not stop the rest of
the list. An undefined name keeps its place (as zero), so the addresses that
follow are unchanged. Other bad items are dropped.

## Deliberate differences from Keil

| Source | Keil | openc166 |
|---|---|---|
| bit number outside 0..15 on a register or SFR (`R1.-1`) | warning A62, wraps | error A67 |
| a bit address that is not bit-addressable (`0FE00H.0`, `10H.2`, `7FH.3`) | silently encodes bit offset 0 | error A103 |
| a plain number used as a bit (`BSET 0FD02H`) | sometimes accepted with an odd meaning | error A103 |
| `n + bit` (`1+0FD00H.3`) | gives bit `0FD00H.9` | error A61; `bit + n` works as in Keil |
| `BIT` or `DEFB` naming a non-bit-addressable address | silently defines `0FD00H.0` | error A103 |
| `-label`, `NOT label`, `-$` | wrong value (`-L` gives `0FFF0H` instead of `0FFFEH` when `L` is 2) | error A63 |
| `-(L1 - L0)`, `NOT (L1 - L0)`, `-(L1 - 1)` | wrong value, no warning | error A63 |
| `number - label` (`10H - L1`) | wrong value | error A63 |
| decimal fraction such as `#1.0` | accepted as 0 | error A70 |
| instruction at an odd address | accepted | error A134 |
| instruction outside a `PROC` | error A45 | accepted |
| instruction or data outside any `SECTION` | error A44 | placed at `--origin` |
| error inside an instruction | sometimes still emits code | emits nothing (no binary is written anyway) |
| `JMPR`/`CALLR` to a distant label in another section | resolved by the linker, which fails there | error A82 |
| relative `$INCDIR` directory | searched from the current directory only | also searched from the main source's directory |
| `$INCLUDE` after another control on the line (`$NOLIST INCLUDE(x)`) | silently ignored | ignored with warning OC7 |
| error A171 after a bad `$INCLUDE` line | reported on the following line | reported on the `$INCLUDE` line |
| missing include file | fatal error, no listing | error OC5; the listing stops at that line |
| `$ABSOLUTE`, `$NOABSOLUTE` | documented, but A16 in V5.38 | accepted and ignored |

Keil's wrong values for negated addresses also appear in the linked program,
so they are real miscompilations, not listing glitches. Keil appears to
multiply the address by 8 before negating it. `-(L1 - L0)` can be written
as `0 - (L1 - L0)`, which both assemblers compute correctly.

Keil leaves some branch targets, such as `JMPR cc_UC, $+4`,
`JMPR cc_UC, label+2`, `JMP label*1`, a label in another section, or a
plain number in a section with `AT`, for its linker to fill in. openc166
resolves them directly, because the target address is known, and picks
the jump form from that address. Keil may pick a different form for these
lines, so the test corpus marks them with `; DIVERGES:`.

In a section without `AT`, Keil compares a plain-number jump target with
the offset inside the section, not with the final address. openc166 always
uses the final address.
