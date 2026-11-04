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

openc166 writes what L166 and OH166 would make of the module, or of several
modules linked together (see [Linking several files](#linking-several-files)): a raw binary,
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
  `SHORT`, and the type operators `NEAR`, `FAR`, `BYTE`, `WORD` and
  `DWORD` (with or without `PTR`). See
  [Expression details](#expression-details) and
  [Segmented mode and DPPs](#segmented-mode-and-dpps).
- **Sections:** `SECTION` with or without `AT`, with Keil's types, align
  types, combine types and class names. Sections without `AT` are placed as
  L166 would place them. See [Sections](#sections). `BIT` sections hold
  bit variables (`DBIT`); see [BIT sections](#bit-sections).
- **Groups:** `DGROUP` and `CGROUP`, which keep sections together in one
  data page or code segment. See [Groups](#groups).
- **Directives:** `SECTION` / `ENDS`, `PROC [NEAR|FAR|INTERRUPT|TASK]`
  / `ENDP`, `LABEL`, `EQU`, `SET`, `BIT`, `DBIT`, `DB`, `DW`, `DD`, `DS`,
  `DSB`, `DSW`, `DSD`, `DPPTR`, `DSPTR`, `DBPTR`, `ASSUME`, `ORG`, `EVEN`
  and `END`.
- **Register definitions:** `DEFR`, `DEFA` and `DEFB`, the directives that
  register files such as `reg167.inc` use. See
  [Register definitions](#register-definitions).
- **Include files:** `$INCLUDE(file)` (short form `$IC`) and
  `$INCDIR(dir;dir...)` (`$ID`). See [Include files](#include-files).
- **Conditional assembly:** `$SET`/`$RESET`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF`,
  the `IF`/`ELSEIF`/`ELSE`/`ENDIF` statements, and `$COND`/`$NOCOND`. See
  [Conditional assembly](#conditional-assembly).
- **Text replacement:** `name LIT 'text'`. See [LIT](#lit).
- **Macros:** `MACRO`/`ENDM`, `LOCAL`, `REPT`, `IRP`, `IRPC`, `EXITM`, the
  `NUL` operator, `&` and `%`. See [Macros](#macros).
- **Several modules:** `EXTERN`/`EXTRN`, `PUBLIC` and `GLOBAL`, with
  sections combined across modules as L166 combines them. See
  [Linking several files](#linking-several-files).
- **Register banks and linker directives:** `REGDEF`/`REGBANK` (named
  banks are placed as L166 places them), `NAME`, `SSKDEF`, `PECDEF` and
  `TYPEDEC`, with Keil's error checks. See
  [Register banks and linker directives](#register-banks-and-linker-directives).
- **Accepted and ignored:** listing and object-file controls such as
  `$NOLIST`, `$TITLE(...)` and `$DEBUG`, which only matter to a printed
  listing or a debugger.
- **Controls:** `$MOD167`, `$MOD166`/`$NOMOD166`, `$CASE`/`$NOCASE`,
  `$SEGMENTED`/`$NONSEGMENTED`, `$TABS`, `$MACRO`/`$NOMACRO`,
  `$MPL`/`$NOMPL`, `$GEN`/`$NOGEN`/`$GENONLY`, `$SAVE`/`$RESTORE`.
  `$MODV2` falls back to the C167 set, with warning OC2. Some controls must
  come before the first statement; see [Controls](#controls).

## Not supported yet

These produce error OC3, so a missing feature is never silently skipped:

- Keil's MPL macro processor: under `$MPL`, which is Keil's default, every
  `%` starts an MPL call, even in comments and strings (see
  [Macros](#macros))
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
`SEGMENTED`/`SG`, `NONSEGMENTED`/`NOSG`, `TABS`/`TA`, `MACRO`, `NOMACRO`,
`MPL`, `NOMPL`. Those it ignores: `PAGELENGTH`/`PL`, `PAGEWIDTH`/`PW`, `SYMBOLS`/`SB`,
`NOSYMBOLS`/`NOSB`, `XREF`/`XR`, `NOXREF`/`NOXR`, `DEBUG`/`DB`,
`NODEBUG`/`NODB`, `PRINT`/`PR`, `NOPRINT`/`NOPR`, `OBJECT`/`OJ`,
`NOOBJECT`/`NOOJ`, `ERRORPRINT`/`EP`, `NOERRORPRINT`/`NOEP`, `PAGING`,
`NOPAGING`, `DATE`, `LINES`, `NOLINES`, `TYPE`, `NOTYPE`, `AMAKE`,
`NOAMAKE`, `USEDEXTONLY`, `EXTMAC`, `EXPDECNUM`, `ABSOLUTE`, `NOABSOLUTE`.

General controls that openc166 uses: `INCLUDE`/`IC`, `SET`, `RESET`, `IF`,
`ELSEIF`, `ELSE`, `ENDIF`, `COND`, `NOCOND`, `GEN`/`GE`, `NOGEN`/`NOGE`,
`GENONLY`/`GO`, `SAVE`/`SA`, `RESTORE`/`RS`. Those it ignores: `LIST`,
`NOLIST`, `TITLE`/`TT`, `EJECT`/`EJ`, `SYMLIST`, `NOSYMLIST`, `REGUSE`.

These lists come from testing A166 V5.38. The manual calls `TITLE` and
`NOCOND` primary, but A166 accepts them anywhere. An unknown control is
A16.

**Contradicting controls.** A primary control that undoes an earlier one,
such as `$NOCASE` after `$CASE` or `$NOMPL` after `$MPL`, is error A7. As
in Keil, the control still takes effect, but the controls after it on the
same line are dropped. Among the CPU controls, `$MOD167` after `$MOD166`
or `$MODV2`, `$MOD166` after `$MODV2`, and `$MODV2` after `$MOD166` are A7;
an earlier `$MOD167` never conflicts. `$MACRO` and `$MPL` are separate
pairs, so `$NOMACRO` after `$MPL` is fine.

**Tabs.** Before a line is read, tabs are expanded to spaces, also inside
strings: `DB '<tab>'` stores spaces. Tab stops are every 8 columns, or
every n columns after `$TABS(n)` (short form `$TA`), with n from 1 to 16.
A value outside that range, or one that is not a number, is A19, and a
missing parenthesis A22; both set the width to 1. Text after the number,
as in `$TABS(2+2)`, is A22, but the number (here 2) is used. Keil does not
evaluate an expression here. The width counts from the line after the
control.

**The MPL setting covers the whole control area.** Keil looks through the
primary-control area for `$MPL`, `$NOMPL` and `$NOMACRO` before it reads
the first line, and the last one found applies from line 1. So a `%` in a
comment above `$NOMPL` is not an MPL call; openc166 does the same.

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

## Macros

openc166 supports Keil's standard macros:

```
$NOMPL                           ; '%' is not an MPL call; see below
LOADW   MACRO   DST, VAL         ; a macro with two parameters
        LOCAL   SKIP             ; SKIP gets a new name in each expansion
        MOV     DST, #VAL        ;; a ';;' comment is not copied
        JMPR    cc_UC, SKIP
SKIP:
        ENDM

        LOADW   R5, 1234H        ; MOV R5, #1234H ...
        REPT    3                ; three NOPs
        NOP
        ENDM
        IRP     REG, <R1, R2>    ; MOV R1, #0 and MOV R2, #0
        MOV     REG, #0
        ENDM
        IRPC    CH, AB           ; DB 'A' and DB 'B'
        DB      'CH'
        ENDM
```

- **Definitions.** `name MACRO [param, ...]` up to `ENDM`. `LOCAL` may
  only be the first line of the body. A macro is used from the line after
  its definition. As in Keil, a macro cannot be defined again:
  `M1 MACRO x`, where `M1` is already a macro, is a call to `M1`. An
  instruction name cannot be a macro name (A6). `MACRO`, `REPT`, `IRP` and
  `IRPC` blocks inside a body are expanded when the body is. A repeated
  parameter name is A123 and the macro is not defined; a `LOCAL` name that
  is also a parameter is A123 too. A missing `ENDM` is A120 (with warning
  A89, since the `END` is then part of the macro), a stray `ENDM` or
  `LOCAL` is A6, and `EXITM` outside a macro is A124. Names follow
  `$CASE`/`$NOCASE`.
- **Expansion.** Each parameter is replaced wherever it stands as a whole
  word in the body, even inside strings and comments. `&` joins a parameter
  to the text around it and is removed (`R&NUM`, `'R&NUM'`). `LOCAL` names
  become `??0000`, `??0001` and so on, counting through the whole file.
  Lines are expanded as they are assembled, so an `IF` inside a macro sees
  the current value of a `SET` symbol, and a macro can call itself while a
  `SET` counter counts down. `EXITM` ends the innermost expansion (the
  whole of a `REPT`, `IRP` or `IRPC`), closing the `IF` blocks opened in
  it. `LIT` names are replaced in call lines and expanded lines.
- **Arguments** are split at commas and trimmed. A missing argument is
  empty (test it with `IF NUL param`), and extra ones are ignored.
  Parentheses do not protect commas. Quotes protect `,` and `;` and stay
  part of the argument. `<...>` protects everything inside it, nests, and
  loses its outer pair; the argument ends at the `>`, so `<a>b` passes `a`
  and `b`. A `>` straight after that closing `>` is skipped (`<a>>b` passes
  `a` and `b`). `!` at the start of an argument passes the one character
  after it as the whole argument (`!,` passes a comma); elsewhere `!` is an
  ordinary character, and there are no escapes inside `<...>`. A `;`
  outside quotes ends the arguments, though a `<...>` argument that has
  already started may run past it. A quote that is never closed is A2,
  reported at the end of the line, and a `<` without `>` is A6.
- **`%` under `$NOMPL`.** An argument that starts with `%` passes the value
  of the expression after it as an unsigned decimal number:
  `LOADW R1, %CNT*2` with `CNT SET 3` passes `6`. The expression may only
  use symbols defined earlier; anything else is A71, and a label is A72
  (the value is then 0). Keil has three faults here, which openc166 copies
  because real code may depend on them, and warns about with OC26:
  - a number in the expression counts as 0, so `%CNT+1` passes `3`, not
    `4`;
  - after a `,` the next argument starts right after the first token of
    the expression: `%CNT, 4` passes `3`, an empty argument and `4`, and
    `%CNT*CNT, 4` passes `9`, `*CNT` and `4`;
  - any other text after the expression starts the next argument at the
    expression itself: `%CNT X` passes `3` and `CNT X`.

  So put a `%` argument last and use only `SET` or `EQU` names in it.
- **MPL.** Keil also has a second, older macro language, MPL (`%DEFINE`,
  `%IF` and so on), which is on by default. Under `$MPL` every `%` starts an
  MPL call, even in comments and strings. openc166 does not support MPL and
  reports such a `%` as OC3. Put `$NOMPL` at the top of the file when `%`
  is meant as text, as most code that uses macros does. `$NOMACRO` turns
  MPL off too; Keil then still processes `MACRO`, `REPT`, `IRP` and `IRPC`
  but reports A6 on their first line, and so does openc166.
- **Counts.** The count of `REPT` is a 16-bit value. A symbol defined
  later is A71, a label is A72, and any error leaves the body out. `IRP`
  with `<>` runs once with an empty value. `IRPC` runs once for each
  character, quotes included, and once with an empty value for an empty
  list.
- **Listing.** Expanded lines are numbered and carry `+1`, `+2`, ... after
  the number for the depth of expansion. Under `$NOGEN` (the default) and
  `$GENONLY` they are hidden; `$GEN` shows them. `$SAVE` and `$RESTORE`
  keep and bring back the `GEN` setting, 10 levels deep (A14 beyond, and
  A13 for `$RESTORE` without `$SAVE`). A problem in an expanded line is
  reported at the line of the outermost call, or, for a `REPT`, `IRP` or
  `IRPC` written in the source, at its `ENDM` line.
- **Limits.** A macro may call itself 9 levels deep; deeper is OC24 (Keil
  hangs at 10). Calls and blocks may be nested 64 levels deep. More than
  1,000,000 lines to assemble in all is OC25 and stops the assembly, so a
  macro that never stops expanding fails quickly.

Keil gets a few rare argument forms wrong, and openc166 does not copy
them. After `!;` Keil passes garbage for the arguments that follow, and
after a `!` at the end of the line it passes the line break. openc166
passes `;` or an empty argument, stops there, and warns with OC27.

Keil's listing has faults that openc166 does not copy; the code bytes are
the same:

- Under `$NOGEN`, Keil still lists the second and later rows of a hidden
  line with more than 4 bytes of code.
- Keil marks some lines after `$SAVE` with a stray `+1`.
- For a hidden expanded line with an error, Keil lists only the
  `*** ERROR` line; openc166 lists the line itself too.
- A `REPT` whose count names a symbol defined later is expanded in pass 2
  only, which skews Keil's listing. (openc166 reports A71 there anyway.)
- After an argument with `!`, Keil often lists the expanded line wrongly.
- A `MACRO` without `ENDM` marks the rest of Keil's listing with `+:`,
  and A120 appears at the top. openc166 reports A120 and A89 on the last
  line.

## openc166's own messages

Messages that Keil does not have use codes starting with `OC`:

| Code | Kind | Meaning |
|---|---|---|
| OC1 | warning | two pieces of code or data overlap in the image |
| OC2 | warning | `$MODV2`: the XC16x/Super10 extensions are not supported |
| OC3 | error | a control or directive that openc166 does not support yet, or a `%` that starts an MPL call under `$MPL` |
| OC4 | | no longer used (was: external symbols, before linking was supported) |
| OC5 | error | an include file cannot be opened; the assembly stops |
| OC6 | error | include files nested too deeply; the assembly stops |
| OC7 | warning | `$INCLUDE` that is not the first control on its line, ignored |
| OC8 | warning | the last line of an include file was joined to the next line |
| OC9 | warning | an `IF`-statement structure that Keil accepts but that is probably a mistake |
| OC10 | warning | a condition that counts as false because only bit 0 is tested (`IF 2`) |
| OC11 | error | a line longer than 65536 characters after LIT replacement |
| OC12 | error | no room for a section without `AT` |
| OC13 | warning | a given section address that does not suit the section's alignment, or names no section |
| OC14 | error | a given section address for a section with `AT`, or one that runs past the address space |
| OC15 | error | an `EXTERN` name that no other module makes `PUBLIC` or `GLOBAL` (L166: L127) |
| OC16 | error | a name that two modules make `PUBLIC` or `GLOBAL` (L166: L104) |
| OC17 | warning | a `NEAR` or `FAR` external that is not a code label in its module (L166: L21) |
| OC18 | error | a use of an external whose value does not suit its type or the place it is used (L166: L121) |
| OC19 | error | parts of a section in different modules that cannot be combined (L166: L101) |
| OC20 | warning | a module whose `$SEGMENTED` setting differs from the first module's (L166: L15) |
| OC21 | error | a `DATA` or `CONST` section without `AT` that is bigger than a 16 KB page (L166: L129) |
| OC22 | error | a `DGROUP` bigger than a 16 KB page or a `CGROUP` bigger than a 64 KB segment (L166: L115, L116), or a section with no room in the page or segment its group is tied to (L112) |
| OC23 | error | `BOF` of a bit in a `BIT` section whose value Keil computes wrongly (see [BIT sections](#bit-sections)) |
| OC24 | error | a macro that calls itself more than 9 levels deep, or macro calls and `REPT`/`IRP`/`IRPC` blocks nested more than 64 levels deep |
| OC25 | error | more than 1,000,000 lines to assemble, usually a macro that never stops expanding; the assembly stops |
| OC26 | warning | a `%` macro argument that Keil reads in a surprising way (see [Macros](#macros)) |
| OC27 | warning | a macro argument `!;` or a `!` at the end of the line, which Keil garbles |
| OC28 | error | a register bank with no room left in `0FC00H..0FDFFH` (L166: L123) |

## Sections

```
name SECTION type [align] [AT address | combine] ['class']
     ...
name ENDS
```

- **Names** must be new. The name of a label, constant, register, PSW flag
  or other predefined symbol gives A25, and a reserved word (a directive,
  type, operator or condition code such as `cc_UC`) gives A6. Either way
  the section is not opened, so its `ENDS` gives A6 and A27, as in Keil.
  Defining a symbol with the name of a section gives A25 too.
- **Types:** `CODE`, `DATA`, `CONST`, `HDATA`, `HCONST`, `XDATA`, `XCONST`
  and `BIT` (see [BIT sections](#bit-sections)).
- **Align types:** `BYTE`, `WORD` (the default), `DWORD`, `PAGE` (16 KB),
  `SEGMENT` (64 KB), `BITADDRESSABLE` and `PECADDRESSABLE`. A type that
  does not suit the section gives A43, as in Keil (`CODE BYTE`,
  `CONST BITADDRESSABLE`).
- **Combine types** (`PUBLIC`, `PRIVATE`, `COMMON`, `GLOBAL`, `SYSSTACK`,
  `USRSTACK`) and **class names** decide how sections of several modules
  are joined; see [Linking several files](#linking-several-files). In a
  single module they make no difference.
- **Reopening** a section continues it. The attributes must match what came
  before: A30 (type), A31 (align type), A32 (combine type or `AT` address),
  A33 (class).
- **Nesting:** open sections form a stack. `ENDS` closes the innermost one.
  As in Keil, a section may be opened again inside itself, and then needs
  one `ENDS` for each opening. `ENDS` with the name of another known
  section gives A29 and still closes the innermost one. With an unknown
  name it gives A28 and closes nothing. With no section open, it gives A6
  and A27.
- **`BYTE` sections without `AT`** may start at any address, so a word
  (`DW`, `DSW`, word labels) in one gives A48, as in Keil.

### Where sections without AT go

Keil leaves the address of a section without `AT` to its linker, L166.
openc166 places these sections itself, as L166 does by
default:

- in order of first appearance;
- each at the lowest free address that suits its align type, from
  `--origin` on (default 0, as in L166), around the sections with `AT` and
  the code outside sections;
- [register banks](#register-banks-and-linker-directives) first, in
  `0FC00H..0FDFFH`; then `BITADDRESSABLE` sections in `0FD00H..0FDFFH`,
  and then `BIT` sections (see [BIT sections](#bit-sections));
- the sections of [groups](#groups) first, then the others;
- `DATA` and `CONST` sections inside one 16 KB data page, and `CODE`,
  `HDATA` and `HCONST` sections inside one 64 KB segment, unless they are
  bigger than that;
- without `$SEGMENTED`, in the first 64 KB;
- empty sections take no room.

`--section NAME=ADDR` (Python: `sections={"NAME": ADDR}`) puts a section at
a given address instead, like L166's `SECTIONS` control. Only sections
without `AT` can be placed this way (OC14). An address that does not suit
the align type, or a name that matches no section, gives warning OC13. A
section that fits nowhere gives OC12.

The listing ends with a table of every section: type, align type, address,
size, whether the address came from `AT`, from `--section`, or from
openc166, and its class and group if it has them.

### Values that move with their section

To Keil, a label in a section without `AT` is not a number yet: the linker
fills it in. That limits what may be computed from it, and it decides some
instruction forms. openc166 follows the same rules, even though it knows
the final address, so that the same source gives the same code and errors.

"Movable" values are labels, `$` and section names in a section without
`AT`, and `PAG`, `POF`, `SOF`, `SEG`, `HIGH`, `LOW`, `BYTEn` and `WORDn`
of these, each plus or minus a plain number.

- **Long forms:** an immediate with a movable value always uses the long
  instruction form (`MOV R1, #POF X` is 4 bytes even if the value is
  small). In a section with `AT` the same expression is a plain number and
  may use the short form.
- **Differences** of two labels in the same section (`Y - X`, or the
  section name minus one of its labels) are plain numbers.
- **Allowed:** comparing two labels of the same section (`X LT Y`), adding
  a difference to a movable value (`POF X + (Y - X)`), and `DATAn` of a
  label in an immediate.
- **Error A63:** any other arithmetic with movable values, for example
  `X AND 1`, `X * 2`, `X SHR 1`, `-POF X`, `NOT PAG X`, `2 - POF X`,
  `X + POF Y`, the difference of labels in different sections, or a
  comparison of a label with a number.
- **Error A72:** a movable value where a number is needed now: `DS X`,
  `EXTR #POF X`, `AT X`. After `AT X` the section is still opened, at
  address 0, as in Keil.
- **Error A104:** `DATAn` of `PAG`, `POF`, `LOW` and similar in an
  immediate. Keil still emits the instruction; openc166 does too.
- **`ORG`** in a section without `AT` takes an offset in the section:
  `ORG $ + 2` and `ORG X + 10H` work as in Keil.

### Groups

```
name DGROUP section, section, ...
name CGROUP section, section, ...
```

A group keeps sections together so that one DPP (for data) or one segment
register value (for code) reaches all of them. A `DGROUP` holds `DATA`
and `CONST` sections, which must then share one 16 KB data page; a
`CGROUP` holds `CODE` sections, which must share one 64 KB segment.

**Placement**, as L166 does it:

- all `DGROUP`s first, then all `CGROUP`s, then the sections outside
  groups;
- a group goes to the first page (or segment) from `--origin` on that has
  room for all its sections. If one of its sections has `AT` or a
  `--section` address, the group goes to that section's page instead;
  with several such sections, the first one listed decides;
- inside its page, the group's sections go largest first, each at the
  lowest free address, so they fill gaps;
- a group that is too big gives OC22 (L166: L115 or L116), and so does
  each section that has no room in the page its group is tied to (L112).

**The group name** stands for the start of its page or segment: `SEG`,
`SOF` and `PAG` work on it, and `MOV R1, #name` gives the page start with
the DPP that `ASSUME` gives the group. `POF` of a group is error A96, as
in Keil. In `ASSUME`, a group and each of its sections are the same
thing: `ASSUME DPP2:name` covers all its sections, and so does `ASSUME`
on any one of them.

**Errors**, as in Keil:

- A25: the group name is already a label, constant, section or group, or
  a later label or section takes it. A group cannot be extended by a
  second line with the same name.
- A53: a section that is already in a group.
- A34: a section of the wrong type. It still joins the group.
- A93: a name that is not a section; A25 if it is a symbol or a group.
- A6: no group name, no sections, or a trailing comma; A39 for `PUBLIC`
  of a group.

A section named before its `SECTION` line is fine.

### BIT sections

```
name SECTION BIT [BIT|BYTE|WORD|DWORD|BITADDRESSABLE] [AT address | combine] ['class']
label:  DBIT [n]
name ENDS
```

A `BIT` section holds one-bit variables in the bit-addressable area
`0FD00H..0FDFFH`, where `BSET`, `BCLR`, `JB` and the other bit
instructions reach them. Its location counter counts bits. The listing
shows it as a word address and a bit, `FD02.3`, as Keil does.

- **`DBIT n`** reserves `n` bits (1 if `n` is left out). Its label is a
  bit, used like `PSW.3`: `BSET Ready`, `JB Ready, Loop`, `BSET Ready+2`.
  `n` must be a plain number known at that point: a name defined later is
  A71, a bit or label A72. `DBIT` outside sections is A44, in another type
  of section A45.
- **Align types:** `BIT` (the default: no gap), `BYTE`, `WORD`, `DWORD`
  and `BITADDRESSABLE`. Others give A43.
- **Bytes and words:** `DB`, `DS` and `DSB` move to the next whole byte;
  `DW`, `DSW`, `DD` and `EVEN` to the next word. `DS n` reserves `n` bytes.
  As in Keil, word data and `EVEN` in a section without `AT` that is
  aligned to bits or bytes give A48, and so does a labelled `DB` in one
  aligned to bits. `ORG` takes a byte address, or an offset in bytes
  without `AT`. Instructions are A45 and `PROC` is A6.
- **`AT address`** takes a byte address or a bit (`AT 0FD10H.3`) in
  `0FD00H..0FDDFH`. Outside that range it is A91, and the section starts at
  `0FD00H.0`, as in Keil. A section with `AT` must end below `0FE00H`, and
  one without `AT` may hold up to 2048 bits; past that, or before its
  start, is A86.
- **Placement**, as L166 does it: first the `BITADDRESSABLE` sections of
  any type, then the `BIT` sections, each in the first gap from
  `0FD00H.0` that holds it, counted in bits. Other sections stay out of
  every byte that bits use. Parts of a `BIT` section in several modules
  join (`PUBLIC`) or overlap (`COMMON`) like other sections, in bits.
- **Names for bits:** `EQU`, `SET` and `BIT` take any bit, `DBIT` labels
  included. As in Keil, `EQU` of a register bit (`R3.4`) gives the bit
  number (4), not a bit. A bit has no bits of its own, so `Ready.1` is
  A69.
- **`BFLDL` and `BFLDH`** take a bit as well as a word, and use the word
  the bit is in: `BFLDL Ready, #3, #1`. A register bit there is A74.
- **The section name** of a `BIT` section with `AT` is its byte address.
  That of one without `AT` is not a value (A70), and `ASSUME` cannot take
  either (A56).
- **`BOF`** of a bit in a `BIT` section works in a section without `AT`,
  for a bit in the word at `0FD00H`. Elsewhere Keil computes a wrong value
  (`BOF` of `0FDD0H.3` is 0, not 3), so openc166 gives OC23. `POF`, `SOF`,
  `PAG` and `SEG` of a bit are A61. Keil rejects most of them too, but
  not all: `DW POF` of `0FD10H.9` gives `0FD11H`.

The section table at the end of the listing shows the address and size of
a `BIT` section in bytes and bits: `00FD02H.3  size 0001H.3` is 11 bits
from bit 3 of `0FD02H`. The `size` that the C and Python interfaces report
counts every byte the section touches; `Section.bit` and
`Section.bit_size` in Python, and `oc166_result_section_bits()` in C, give
the first bit and the size in bits.

## Register banks and linker directives

```
name REGBANK R0-R7          ; or REGDEF; a list such as R0, R4-R6
     SSKDEF  3              ; system stack size
     PECDEF  PECC0-PECC3    ; PEC channels in use
     NAME    MYMODULE
```

These tell the linker about the module. In Keil they mostly end up in the
object file; openc166 checks them as Keil does and places named register
banks.

- **A named register bank** reserves room for the registers `R0` up to the
  highest one listed: `RB REGBANK R0-R3` takes 8 bytes, and so does
  `RB REGBANK R3`. Like L166, openc166 places it as a `DATA` section of
  class `*REG*`:
  - before all other sections without `AT`, at the lowest free even
    address from `0FC00H` on;
  - banks of the same name in several modules become one, as big as the
    biggest;
  - a bank with no room left below `0FE00H` is error OC28 (L166: L123).

  `REGDEF` without a name only declares the registers and reserves
  nothing. An empty list is fine with a name (no registers) and A6
  without one. A range backwards (`R3-R1`) is A52; anything that is not a
  register list is A6.
- **The bank name** is the bank's address, so `MOV R1, #RB` followed by
  `MOV CP, R1` switches to it. Keil knows that a bank lies in page 3:
  `PAG RB` is the plain number 3, and no `ASSUME` is needed to reach it.
  `SEG RB` is 0 in `$SEGMENTED` mode; otherwise it is left to the linker,
  as in Keil (so it takes the long instruction form).
- **Errors for bank names**, as in Keil: a name that is already a symbol,
  section or group is A5. Using a bank name as a section, group or label
  afterwards is A25 (A5 for `name LABEL`). `PUBLIC` is A39, `EXTERN
  x:REGBANK` A113, `ASSUME DPPn:bank` A56, `bank.n` A69, `BOF bank` A61.
  After some of these errors, Keil quietly turns the bank name into the
  failed label, constant or procedure. openc166 keeps it as the bank, so
  later lines that use the name can differ; the module has errors anyway.
- **`SSKDEF n`** takes a number known at that point: 0..3, or 0..7 with
  `$MOD167` (otherwise A66). A name defined later is A71, a label or bit
  A72, a register A70.
- **`PECDEF`** takes `PECC0` to `PECC7`, alone or as ranges, in any order;
  anything else is A88.
- A module may have one `SSKDEF` and one `PECDEF`: a second one is A87.
  As in Keil, an `SSKDEF` whose value is out of range does not count, but
  one with any other error does.
- **`NAME name`** names the module. A second `NAME` is A25; a reserved word
  is A6 and A23.
- **`TYPEDEC`** declares types for a debugger. openc166 accepts and
  ignores everything after it; a name in front is A6.
- **`COMREG`** is not a Keil A166 directive (Keil gives A6), and neither
  is it one in openc166.

## Linking several files

Keil assembles each source file into an object file with A166, then joins
the object files with its linker, L166. openc166 does both in one step when
it gets several sources: `openc166 main.a66 lib.a66`, `assemble_many()` in
Python, `oc166_assemble_many()` in C. Each source is a **module**. It is
assembled on its own, as A166 would, and the modules are then joined as
L166 joins them by default. The link cases in `tests/diff/cases/link/`
compare the result, and the linker's errors, with L166's.

Messages and the listing name the module they belong to. `$INCLUDE`
searches the directory of the module's own file first. Each module chooses
its instruction set with `$MOD167` (or `--cpu`); the first module decides
segmented or not for the program, as in L166 (OC20 for a module that
differs, L15 in L166).

### Sharing symbols

- **`PUBLIC name, ...`** and **`GLOBAL name, ...`** let other modules use
  names of this one: labels, `PROC` names, `EQU`, `BIT`, `DEFR`, `DEFA`
  and `DEFB` symbols, and predefined registers. As in Keil, they are
  checked at the end of the module, with errors on the `PUBLIC` line: A42
  for an unknown name, and A39 for a `SET` symbol, a `LIT`, a section name,
  a reserved word, an `EXTERN` name or a value computed from one. Naming a
  symbol both `PUBLIC` and `GLOBAL` gives A25. Otherwise openc166 treats
  `PUBLIC` and `GLOBAL` names the same.
- **`EXTERN [DPPn:]name:type, ...`** (or `EXTRN`) uses a name that another
  module exports. The types are `NEAR`, `FAR`, `BYTE`, `WORD`, `DWORD`,
  `BIT`, `BITWORD`, `DATA3`, `DATA4`, `DATA8`, `DATA16` and `INTNO`. The
  type, not the value, decides the code, as in Keil:
  - `CALL` and `JMP` to a `NEAR` external are `CALLA` and `JMPA`; to a
    `FAR` one, `CALLS` and `JMPS`;
  - only `DATA3` and `DATA4` externals use the short immediate forms;
  - memory operands use the long forms;
  - in `$SEGMENTED` mode, `DPPn:` says which data page pointer the
    address goes through (without it, warning A77 and DPP0, as in Keil);
    otherwise `DPPn:` gives warning A55 and is ignored.

  As in Keil, an error stops the rest of the list, except a `REGBANK`
  type (A113). An external counts as a value that moves with its section
  (see [above](#values-that-move-with-their-section)): `#Ext * 2` and
  `ORG Ext` give A63, `DS Ext` gives A72.
- **Unresolved names:** an `EXTERN` name that no other module exports is
  error OC15, even if it is never used, as L166 gives L127. A name that two
  modules export is error OC16 in the second (L104).
- **Wrong types:** a `NEAR` or `FAR` external that is not a code label in
  its own module gives warning OC17 (L21). A value that does not suit the
  type gives error OC18 at each place it is used, as L166 gives L121 at
  each fixup: a `BIT` external that is not a bit, a `BITWORD` external that
  is not a bit-addressable word, a `DATA4` external of 20, an `INTNO` over
  7FH, a `DB` of 300H. Word-sized externals are cut to 16 bits without a
  message, as in L166.
- Code bytes in the listing show the final values of externals, where
  Keil's listing shows zeros for the linker to fill in.

### Joining sections

- A section is known by its **name and class**. `Code SECTION CODE 'ROM'`
  in two modules is one section; with different classes they are two
  separate sections that happen to share a name.
- Parts of a section in several modules are joined if they all have the
  same type and the same combine type, which is `PUBLIC`, `GLOBAL`,
  `COMMON`, `SYSSTACK` or `USRSTACK`. Each part is compared with the first
  one, as in L166: a part that does not match is error OC19 (L101) and
  stays a section of its own; the others still join. `PRIVATE` (the
  default) and sections with `AT` are never joined.
- **`PUBLIC`, `GLOBAL`, `SYSSTACK` and `USRSTACK`** parts follow one
  another in module order, each aligned as its own module asks: a 1-byte
  `BYTE` part followed by a `WORD` part puts the second one at offset 2.
- **`COMMON`** parts lie on top of each other. The section is as long as
  its longest part, and where parts overlap, the bytes of the later module
  win, as in L166. This gives no OC1 warning.
- The joined sections are placed as described in
  [Where sections without AT go](#where-sections-without-at-go), in order
  of first appearance over all modules. A `DATA` or `CONST` section
  without `AT` that ends up bigger than a 16 KB page, in one module or
  joined from several, is error OC21 (L129).
- Groups of the same name in several modules are one group, holding the
  sections of all of them.
- Code outside sections starts at `--origin` in every module, so it
  overlaps (OC1) if more than one module has some. Keil does not allow
  such code at all (A44).

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
colon label inside a `FAR` procedure is still NEAR.

**Type operators.** `NEAR`, `FAR`, `BYTE`, `WORD` and `DWORD`, with or
without `PTR`, change the type of an address: `FAR x` and `FAR PTR x` are
the same. `SHORT x` makes `JMP` and `CALL` use the relative form. Like
`HIGH` or `DPPn:`, these bind to the operand right after them, so
`FAR x + 2` means `(FAR x) + 2`. When several are stacked, the outermost one
wins: `JMP FAR SHORT x` is `JMPS`, `JMP SHORT FAR x` is `JMPR`. On a plain
number they do nothing (`JMP FAR PTR 1234H` is `JMPA`).

**Label plus a number.** In a section without `AT`, `label + n` and
`label - n` are still addresses and keep the label's type (and `SHORT`):
`JMP FarProc + 2` is `JMPS`, and `JMP Table + 2` warns A74 if `Table` is a
data label. In a section with `AT`, `label ± n` with `n` not zero is a plain
number to Keil, so it has no type: `JMP FAR Here + 2` is `JMPA`.

**Forward references.** Keil refuses to compute with a type operator
applied to a symbol that is defined further down: `FAR Later + 2` gives
A61, and `*`, `/`, `MOD`, `SHL`, `SHR`, `AND`, `OR` and `XOR` give A63. A
comparison is fine only if the other side also refers to a later symbol.
Parentheses around the operator do not help, but arithmetic inside the
operand does: write `FAR (Later + 2)`.

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
for a near jump or call to a FAR label, A134 for an odd target. Like Keil,
openc166 does not check the alignment of a target that the linker places (a
label in a section without `AT`, plus or minus a number).

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
  `target`: a section, a [group](#groups), a label in a section, `SYSTEM`
  (the SFR page) or `NOTHING`. Memory operands, immediates and `[Rw+#x]` displacements that
  name a label of that section (or an SFR, for `SYSTEM`) then use DPP *n*.
  With `DS2` at `12340H` and `ASSUME DPP2:DS2`, `MOV R1, W1` (W1 at the
  start of DS2) gives `F2F140A3`.
- A section, group, or `SYSTEM` can belong to one DPP only (error A94).
  A group and its sections count as one.
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
  Keil does not check addresses that the linker places (labels in sections
  without `AT`), so neither does openc166.
- `DS` and `ORG` check how far they move the location counter (error A86,
  "section length overflow"), with the same limits as Keil:
  - a `DATA` section with `AT` must fit in the 16 KB page it starts in;
  - a `CONST` section must leave the last byte of its 16 KB page free (one
    without `AT` may be at most 3FFFH bytes long);
  - a `CODE`, `HDATA` or `HCONST` section may be at most 64 KB long;
  - `DATA` sections without `AT`, `XDATA` and `XCONST` are not checked
    here.

  `DB`, `DW` and the other data directives never trigger A86. A `DS` that
  does still reserves its space; an `ORG` that does is ignored.

## Expression details

**Operator precedence**, from tightest to loosest, as in Keil:

1. unary `-`, `NOT`, `HIGH`, `LOW`, `SEG`, `SOF`, `PAG`, `POF`, `DATA3`,
   `DATA4`, `DATA8`, `DATA16`, `DPPn:`, `SHORT`, and the type operators
   `NEAR`, `FAR`, `BYTE`, `WORD`, `DWORD` (with or without `PTR`);
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
`BOF PSW.3` means `(BOF PSW).3` and fails with A61. For bits in `BIT`
sections, see [BIT sections](#bit-sections).

**Bit bases.** In `base.n`, the base is an SFR, a GPR, a bit-addressable
address or a label in a `BITADDRESSABLE` section. As in Keil, the name of a
section without `AT`, a group or a register bank is A69 (the name of a
section with `AT` is its address, and works). `SOF`, `POF`, `PAG` or `SEG`
of a label is A70 and A68, as the linker fills in that number.

**`BYTE0`..`BYTE7` and `WORD0`, `WORD2`, `WORD4`, `WORD6`** pick a byte or a
16-bit word of a value: `BYTE1 12345678H` is 56H, `WORD2 12345678H` is
1234H. Only these names are operators and reserved words. Other names that
look alike, such as `WORD3` or `BYTE8`, are ordinary symbols, as in Keil.

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
| `IF` condition with a label, register, SFR, bit or external | silently false | error A11 |
| stray `ENDIF`, second `ELSE`, `ELSEIF` after `ELSE`, text after a condition | silently accepted | accepted with warning OC9 |
| two conditional keywords on one line (`ELSE ELSE`, `ENDIF IF 0`) | sometimes A6, sometimes silent; can leave the nesting confused | error A6; the line is ignored |
| `$INCLUDE` of a missing file after `END` | fatal error | ignored, like every line after `END` |
| `$INCLUDE(REG167.INC)` (or another name Keil ships) | Keil's own `C166\ASM` directory is searched right after the current directory, before the source directory | the built-in copy is used only when the name is found nowhere else |
| repeated definition with a different value (A25) | the new value is used from then on | the first value stays |
| `LIT` that fails with A25 (`E1 LIT 'R1'` after `E1 EQU 3`) | the text is still used, in the second pass even on the lines before it (`E1 EQU 3` becomes `R1 EQU 3`) | the LIT is not defined |
| name used before its `LIT` | A60; the listing shows the line replaced | A60; the listing shows the line as written |
| `X - FX`, with `X` in a section without `AT` and `FX` in one with `AT` | silently 0 | error A63 |
| `X + X`, with `X` in a section without `AT` | twice the offset of `X` in its section | error A63 |
| `POF X - POF Y`, `SEG X + SEG X` (sections without `AT`) | silently 0 | error A63 |
| `IF X EQ 2`, with `X` in a section without `AT` | compares the offset in the section | error A63 |
| `EXTERN PSW:WORD`, or another predefined register as an external | accepted | error A25 |
| sections with `AT` and the same name in two modules | L166 error L113 | error OC19 (and OC1 if they overlap) |
| code outside sections in several modules | error A44 | each module's starts at `--origin`; overlaps give OC1 |
| `EQU` or `SET` with the name of a group | A166 hangs | error A25 |
| `BOF` of a bit in a `BIT` section with `AT`, or outside the word at `0FD00H` in one without | wrong value | error OC23 |
| `POF`, `SOF`, `PAG` or `SEG` of a bit | A45 or A70 in most places, but `DW POF` of `0FD10H.9` gives `0FD11H` and `EQU` gives a bit with no value | error A61 |
| a macro that calls itself 10 or more levels deep | A166 hangs | error OC24 at the 10th level |
| macro argument `!;` | garbles the arguments that follow | passes `;`, ignores the rest, warning OC27 |
| macro argument `!` at the end of the line | passes the line break | passes an empty argument, warning OC27 |
| `%CNT.1` in a macro argument | passes a number (64768) | error A103 |
| `$OBJECT` without a file name | A22 | accepted and ignored |

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
