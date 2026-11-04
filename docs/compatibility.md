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
the complete memory image with openc166's Intel HEX output.

openc166 writes what L166 and OH166 would make of the module: a raw binary,
or Intel HEX in the format of `OH166 ... H167` (type 04 records for
addresses above 64 KB; OH166 without `H167` writes type 02 records, which
hold the same data). Records may break at different places than OH166's,
since OH166 starts a new record at each section.

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
  `HIGH LOW SOF SEG PAG POF BOF`, `DATA3 DATA4 DATA8 DATA16`, `$`, section
  names, character constants, bit selection (`base.n`), `DPPn:` prefixes,
  and the `SHORT` and `type PTR` operators. See
  [Expression details](#expression-details) and
  [Segmented mode and DPPs](#segmented-mode-and-dpps).
- **Directives:** `SECTION ... [AT addr]` / `ENDS`, `PROC [NEAR|FAR|INTERRUPT|TASK]`
  / `ENDP`, `LABEL`, `EQU`, `SET`, `BIT`, `DB`, `DW`, `DD`, `DS`, `DSB`, `DSW`,
  `DSD`, `DPPTR`, `DSPTR`, `DBPTR`, `ASSUME`, `ORG`, `EVEN` and `END`.
- **Register definitions:** `DEFR`, `DEFA` and `DEFB`, the directives that
  register files such as `reg167.inc` use. See
  [Register definitions](#register-definitions).
- **Include files:** `$INCLUDE(file)` (short form `$IC`) and
  `$INCDIR(dir;dir...)` (`$ID`). See [Include files](#include-files).
- **Conditional assembly:** `$SET`/`$RESET`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF`,
  the `IF`/`ELSEIF`/`ELSE`/`ENDIF` statements, and `$COND`/`$NOCOND`. See
  [Conditional assembly](#conditional-assembly).
- **Text replacement:** `name LIT 'text'`. See [LIT](#lit).
- **Accepted and ignored:** these only matter to a linker or debugger.
  - Directives: `NAME`, `PUBLIC`, `GLOBAL`, `REGDEF`, `REGBANK`, `SSKDEF`,
    `PECDEF`, `TYPEDEC`.
  - Listing and object-file controls: `$NOLIST`, `$TITLE(...)`, `$DEBUG`,
    and similar.
- **Controls:** `$MOD167`, `$MOD166`/`$NOMOD166`, `$CASE`/`$NOCASE`,
  `$SEGMENTED`/`$NONSEGMENTED`.
  `$MODV2` falls back to the C167 set, with warning OC2. Some controls must
  come before the first statement; see [Controls](#controls).

## Not supported yet

These produce error OC3 (or OC4 for externals), so a missing feature is never
silently skipped:

- `MACRO` and the other macro facilities
- `EXTERN`/`EXTRN` (a raw image has no linker to resolve them)
- `DBIT`, `DGROUP`, `CGROUP`, `COMREG`, and BIT sections
- the XC16x/Super10 extended instruction set

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

**Repeating a definition.** Register files define some names that already
exist: the core registers (`PSW`, `MDC`, `CP`, ...), the PSW bits (`C`, `Z`,
`IEN`, ...), and sometimes a name twice. As in Keil, these may repeat:

- `DEFR` for a name that is already an SFR **at the same address** (the
  access attribute may differ);
- `BIT` for a name that is already a bit **at the same bit address** (from
  `BIT`, `DEFB` or a built-in name).

Every other repeat is error A25: a different address, a repeated `DEFA`,
`DEFB` or `EQU` (even with the same value), `DEFB` after `BIT`, a change of
kind (`DEFR` then `EQU`), and `DEFR` for a GPR name such as `R0`.

## Register file REG167.INC

C167 programs usually start with

```
$MOD167
$INCLUDE(REG167.INC)
```

to get names for the C167's registers and bits (`SYSCON`, `T3CON`, `T3IR`,
`ADST`, ...). openc166 has its own [`inc/REG167.INC`](../inc/REG167.INC),
built into the program, so this works without any extra file. When a file
called `REG167.INC` is found on the [search path](#include-files), that
file is used instead, for example Keil's own.

The file was written from Infineon's documentation, not copied from Keil:

- every register of the C167CR User's Manual V3.1, including the core
  registers, and `SYSCON2`, `SYSCON3` and `ISNC` from the C167CS data sheet;
- a name for every single-bit field in the register descriptions, such as
  `T3R` or `ADST`; a field of several bits (`T3I`, `ADCH`) has no name, so
  use `T3CON.0` and so on;
- `xxIR` (bit 7) and `xxIE` (bit 6) for every interrupt control register
  `xxIC`;
- the port pins that the timers use: `T0IN`, `T2IN`, `T3IN`, `T4IN`, `T3EUD`,
  `T3OUT`, `CAPIN`, `T6OUT` on P3, and `T2EUD`, `T4EUD`, `T5EUD`, `T6EUD`,
  `T5IN`, `T6IN` on P5;
- `SGTEN` (= `SGTDIS`, SYSCON.11) and `SSCP0` (= `SSCPO`, SSCCON.6), other
  spellings that existing sources use.

Every name that also appears in Keil's `REG167.INC` has the same address;
this was checked name by name. Keil's file has some names that ours lacks:
the bits of the C167CS registers `SYSCON2`, `SYSCON3` and `ISNC` (their
layout is not in any document we have), and the C167CS registers `SCUSLC`
and `SCUSLS`. Use `SYSCON3.4` and so on, or a `BIT` line of your own. Ours
has some names Keil's lacks, mostly bits that the manual names but Keil does
not (`VISIBLE`, `SWR`, `CT3`, ...).

Without `$MOD167` the file fails (with Keil's file too): the extended SFRs
do not exist on the C166, and some C167 registers moved.

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
4. directories given with `$INCDIR(dir;dir...)`, in order;
5. for a bare file name such as `REG167.INC` (no directory part): the copy
   built into openc166, see [Register file REG167.INC](#register-file-reg167inc).

Keil also searches its own `C166\ASM` directory, which holds Keil's register
files, right after the current directory. openc166 has no such directory;
its built-in copy comes last, so a `REG167.INC` next to the source wins.

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
`NOMOD166`/`NOM166`, `MODV2`, `CASE`, `NOCASE`, `INCDIR`/`ID`,
`SEGMENTED`/`SG`, `NONSEGMENTED`/`NOSG`. Those it ignores: `PAGELENGTH`/`PL`, `PAGEWIDTH`/`PW`, `SYMBOLS`/`SB`,
`NOSYMBOLS`/`NOSB`, `XREF`/`XR`, `NOXREF`/`NOXR`, `DEBUG`/`DB`,
`NODEBUG`/`NODB`, `PRINT`/`PR`, `NOPRINT`/`NOPR`, `OBJECT`/`OJ`,
`NOOBJECT`/`NOOJ`, `ERRORPRINT`/`EP`, `NOERRORPRINT`/`NOEP`, `PAGING`,
`NOPAGING`, `TABS`, `DATE`, `LINES`, `NOLINES`, `TYPE`, `NOTYPE`, `AMAKE`,
`NOAMAKE`, `USEDEXTONLY`, `EXTMAC`, `MACRO`,
`NOMACRO`, `MPL`, `NOMPL`, `EXPDECNUM`, `ABSOLUTE`, `NOABSOLUTE`.

General controls that openc166 uses: `INCLUDE`/`IC`, `SET`, `RESET`, `IF`,
`ELSEIF`, `ELSE`, `ENDIF`, `COND`, `NOCOND`. Those it ignores: `LIST`,
`NOLIST`, `TITLE`/`TT`, `EJECT`/`EJ`, `GEN`, `NOGEN`, `GENONLY`/`GO`,
`SYMLIST`, `NOSYMLIST`, `SAVE`, `RESTORE`, `REGUSE`.

These lists come from testing A166 V5.38. The manual calls `TITLE` and
`NOCOND` primary, but A166 accepts them anywhere. An unknown control is
A16.

**The end of the source.** As in Keil, the lines after `END` are listed but
otherwise ignored, even controls and conditionals. Problems found at the end
(a missing `END`, A89; an unclosed `IF`, A12) are reported on the last line
of the file; an unclosed `SECTION` or `PROC` (A106) on the `END` line.

## Conditional assembly

Keil has two ways to leave lines out, and openc166 supports both:

```
$SET (DEBUG, LEVEL = 2)          ; flags for $IF
$IF (DEBUG AND (LEVEL > 1))      ; parentheses needed, see below
        CALL    trace
$ELSEIF (DEBUG)
        NOP
$ELSE
$ENDIF

COUNT   EQU     4                ; ordinary symbols for IF
        IF      COUNT = 4
        DW      1
        ELSEIF  COUNT > 4
        DW      2
        ELSE
        DW      3
        ENDIF
```

- **`$IF` sees only `$SET` flags.** `$SET (name [= number], ...)` sets
  flags; a name without a value is 1, and `$RESET (name, ...)` sets it to
  0. Any other name in a `$IF` condition, including an `EQU`
  symbol, counts as 0. Flags are not symbols, so an `IF` statement cannot
  see them (A71).
- **`IF` sees `EQU` and `SET` symbols** defined on earlier lines. A
  symbol defined later is A71. A label, register, SFR or bit is error A11
  in openc166; Keil silently treats such a condition as false.
- **Only bit 0 counts.** Keil tests only the lowest bit of a condition, so
  `IF 2` is false. This keeps `NOT` working on 0/1 flags (`NOT 1` has
  bit 0 clear, so it is false) but surprises anyone who expects "non-zero
  is true". openc166 gives warning OC10 for such values, except those
  ending in `FFFEH` like `NOT 1`.
- `=` compares, like `EQ`. Comparisons give 1 (true) or 0. Unlike C,
  `AND` binds tighter than a comparison, so `DEBUG AND LEVEL > 1` means
  `(DEBUG AND LEVEL) > 1`; see [Expression details](#expression-details).
- **One stack.** `$IF` blocks and `IF` statements share one nesting stack,
  so `$ENDIF` can close an `IF` and the other way round. `$IF` allows 9
  levels (A6 beyond that); `IF` statements have no limit.
- **Errors.** A condition with an error counts as false. A stray or
  second `$ELSE` and a stray `$ENDIF` are A17; an `IF` that is still open
  at the end is A12. Keil accepts odd `IF`-statement structures without a
  word, such as a stray `ENDIF`, a second `ELSE` (its lines are assembled
  too when no branch was true) or text after a condition; openc166 accepts
  them too but warns with OC9.
- **Listing.** Lines that are left out appear in the listing without a
  line number, as in Keil. A `$IF`-style line gets a number when the lines
  after it are assembled, an `IF` statement when the lines before it are.
  `$NOCOND` leaves out every line after which assembly is off; `$COND`
  (the default) shows them again.

## LIT

`name LIT 'text'` (or `"text"`) makes `name` stand for a piece of text.
Before a line is read, every whole-word use of the name is replaced by the
text, which is replaced again if it holds other LIT names:

```
PORT    LIT     'R2'
CNT     LIT     '1+2'
        BSET    PORT.3           ; read as BSET R2.3
        MOV     R1,#CNT*2        ; read as MOV R1,#1+2*2, which is 5, not 6
```

- **Where.** Anywhere a name can stand: the mnemonic, a directive, the
  name in front of a directive (`NM EQU 5` defines whatever `NM` stands
  for), a label, operands, and the conditions of `IF` and `$IF`/`$SET`.
  Strings and comments are left alone. The listing shows the replaced
  line. Names follow `$CASE`/`$NOCASE` like other symbols.
- **Not in file names.** `$INCLUDE (name)` and `$INCDIR (name)` open the
  file or directory as written, although Keil's listing shows the name
  replaced. openc166 does the same.
- **Order.** A LIT works from the line after it, also in later passes. A
  name used before its LIT, or a LIT whose text leads back to itself
  (`A1 LIT 'B1'`, `B1 LIT 'A1'`), is error A60. A LIT inside an `IF` block
  that is left out is not defined.
- **The text is raw.** A `;` in the text starts a comment where the text
  lands, and an empty text (`''`) is allowed.
- **Errors.** Defining a name that already exists is A25; so is a LIT
  whose name is itself replaced by a register name. A missing or unquoted
  text, or anything after the closing quote, is A6.

**Labels.** As in Keil, a colon label cannot stand in front of a name and
a directive (`X1: ABC EQU 5`, also `SET`, `LIT`, `PROC`, `DW` and the
like). Inside a section the label is still defined and the rest of the line
is ignored, with A6 at the name and A6 at the directive. Outside a section
it is a single A6 and nothing is defined.

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
| OC9 | warning | an `IF`-statement structure that Keil accepts but that is probably a mistake |
| OC10 | warning | a condition that counts as false because only bit 0 is tested (`IF 2`) |
| OC11 | error | a line longer than 65536 characters after LIT replacement |

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

## Segmented mode and DPPs

**Background.** An instruction such as `MOV R1, mem` holds a 16-bit data
address. On the C166 the top two bits of that address choose one of four
*data page pointer* registers, `DPP0` to `DPP3`, and the other 14 bits are
an offset inside the 16 KB page that the register points to. So the bytes
of the instruction depend on which DPP the program will have pointed at
the variable's page when the instruction runs.

**Nonsegmented mode** (the default) uses the low 16 bits of an address as
they are. Keil warns about the leftovers and openc166 does the same:

- a `DPPn:` prefix is ignored with warning A55;
- `ASSUME` is ignored with warning A54;
- `SECTION ... AT` above `0FFFFH` is error A19.

**Segmented mode** (`$SEGMENTED`, short `$SG`) lets the source say which
DPP to use:

- **`DPPn:address`** gives the DPP number in bits 15–14 and the offset
  (`address AND 3FFFH`) below it: `MOV R1, DPP2:1234H` reads `9234H`.
- **`ASSUME DPPn:target, ...`** says that DPP *n* will point to the page of
  `target`: a section, a label in a section, `SYSTEM` (the SFR page) or
  `NOTHING`. Memory operands, immediates and `[Rw+#x]` displacements that
  name a label of that section (or an SFR, for `SYSTEM`) then use DPP *n*.
  With `DS2` at `12340H` and `ASSUME DPP2:DS2`, `MOV R1, W1` (W1 at the
  start of DS2) gives `F2F140A3`.
- A section, or `SYSTEM`, can belong to one DPP only (error A94).
  `ASSUME NOTHING` forgets all of them. An unknown name is silently
  ignored, as in Keil.
- A label or SFR with no `ASSUME` for its section gives warning A77, and
  its low 16 bits are used. Inside an `EXTP`/`EXTPR` sequence the
  instruction supplies the page, so there is no A77.
- `EQU` and `SET` keep the `ASSUME` link only for a plain label
  (`X SET W1`). A section name or `W1+2` becomes a plain number.

**Data directives** (`DB`, `DW`, `DD`, `DPPTR`, `DSPTR`) store a label's
plain address, even after `ASSUME`. A data section's *name* is different:
alone, in parentheses, or plus or minus a number, it uses the DPP that
`ASSUME` gives the section. With the example above, `DW DS2` stores
`0A340H`, but `DW W1` stores `2340H` (with warning A62, as the value does
not fit in 16 bits). A `DPPn:` prefix in data works on numbers and section
names; a label keeps its address.

**Pointer directives** store an address split for code that loads a DPP or
segment register from a table:

| Directive | Words stored |
|---|---|
| `DPPTR addr` | page offset (`addr AND 3FFFH`), page number (`addr SHR 14`) |
| `DSPTR addr` | segment offset (`addr AND 0FFFFH`), segment number (`addr SHR 16`) |
| `DBPTR bit` | bit number, then the `DPPTR` words of the bit's word address |

**Other checks.**

- A word access to an odd address (`MOV R1, 0E001H`) gives warning A134.
- A `DATA` section with `AT` must fit in the 16 KB page it starts in
  (error A86). Keil also checks the size of some `CODE`, `CONST` and
  `HDATA` sections; openc166 does not yet.

## Expression details

**Operator precedence**, from tightest to loosest, as in Keil:

1. unary `-`, `NOT`, `HIGH`, `LOW`, `SEG`, `SOF`, `PAG`, `POF`, `DATA3`,
   `DATA4`, `DATA8`, `DATA16`;
2. `*`, `/`, `MOD`;
3. `+`, `-`;
4. `SHL`, `SHR`;
5. `AND`, `OR`, `XOR` (one level, left to right);
6. comparisons: `EQ`/`=`, `NE`/`<>`, `LT`/`<`, `LE`/`<=`, `GT`/`>`,
   `GE`/`>=`, and the unsigned `ULT`, `ULE`, `UGT`, `UGE`.

Unlike C, `A AND B > 1` means `(A AND B) > 1`, and `A OR B AND C` means
`(A OR B) AND C`. Use parentheses.

**`BOF`** gives the position (0..15) of a bit, and applies only to the name
or the parenthesised expression right after it: write `BOF (PSW.3)`, as
`BOF PSW.3` means `(BOF PSW).3` and fails with A61.

**`DATA3`, `DATA4`, `DATA8` and `DATA16`** say how many bits an immediate
needs. The value is cut to that many bits without a warning (`#DATA3 -1` is
7, `#DATA8 1234H` is 34H), and the instruction uses the shortest form with
at least that many bits: `MOV R1, #DATA4 3` is 2 bytes, but
`ADD R1, #DATA4 3` is 4, as the short `ADD` has only 3 bits. Keil has no
`DATA32` or `DATA64` (error A6).

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
| `IF` condition with a label, register, SFR or bit | silently false | error A11 |
| stray `ENDIF`, second `ELSE`, `ELSEIF` after `ELSE`, text after a condition | silently accepted | accepted with warning OC9 |
| two conditional keywords on one line (`ELSE ELSE`, `ENDIF IF 0`) | sometimes A6, sometimes silent; can leave the nesting confused | error A6; the line is ignored |
| `$INCLUDE` of a missing file after `END` | fatal error | ignored, like every line after `END` |
| `$INCLUDE(REG167.INC)` (or another name Keil ships) | Keil's own `C166\ASM` directory is searched right after the current directory, before the source directory | the built-in copy is used only when the name is found nowhere else |
| repeated definition with a different value (A25) | the new value is used from then on | the first value stays |
| `LIT` that fails with A25 (`E1 LIT 'R1'` after `E1 EQU 3`) | the text is still used, in the second pass even on the lines before it (`E1 EQU 3` becomes `R1 EQU 3`) | the LIT is not defined |
| name used before its `LIT` | A60; the listing shows the line replaced | A60; the listing shows the line as written |

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
