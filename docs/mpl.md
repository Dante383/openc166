# MPL, Keil's macro processing language

Keil A166 has two macro languages. The usual one is `MACRO`/`ENDM` (see
[compatibility.md](compatibility.md#macros)). The other, older one is MPL,
the Macro Processing Language. openc166 supports both.

## What MPL does

MPL runs over the source **before** the assembler reads it, like a text
filter. Every `%` starts an MPL call:

```
%*DEFINE (LOAD(REG, VALUE)) (MOV %REG, #%VALUE)
%SET (COUNT, 3)

 %LOAD(R1,1234H)            ; the assembler sees:  MOV R1, #1234H
 DW %EVAL(%COUNT * 100)     ; the assembler sees:  DW 012CH
```

The listing shows the text after MPL has run, as Keil's does. Lines with a
`%DEFINE` or `%SET` on their own become blank lines.

MPL is on by default in Keil, so **every `%` counts, even in comments and
strings**. `; 50% done` calls a macro named `DONE`. If a file uses `%` as
plain text, put `$NOMPL` at the top. `$NOMACRO` also turns MPL off.

The TASKING dialect has no MPL: there `%` is the remainder operator.

## What is supported

Everything in Keil's MPL:

- **Macros:** `%DEFINE` and `%*DEFINE`, with parameters, `LOCAL` names
  (`??NAME?n`, numbered as Keil numbers them) and call patterns with
  other delimiters (`%*DEFINE (CP A @TO B) (...)`, called as
  `%CP R1 TO R2`). `%DEFINE` expands the body once, when it is defined;
  `%*DEFINE` expands it at each call. `%*NAME` gives the body without
  expanding it. Arguments keep their blanks: `%LOAD(R1, 1234H)` gives
  `MOV R1, # 1234H`. A last parameter that ends at a blank also takes
  that blank, even a line end: `%CP R1 TO R2` at the end of a line joins
  the next line onto it, as in Keil. Put something after it, such as a
  comment (`%CP R1 TO R2 ; copy`).
- **Variables and numbers:** `%SET (NAME, expression)` and
  `%EVAL (expression)`.
- **Text:** `%LEN`, `%SUBSTR`, and the comparisons `%EQS`, `%NES`, `%LTS`,
  `%LES`, `%GTS`, `%GES` (they give `1` or `0`).
- **Control:** `%IF (..) THEN (..) ELSE (..) FI`, `%WHILE`, `%REPEAT`,
  `%EXIT`.
- **Other:** `%MATCH`, `%METACHAR` (use another character instead of
  `%`), comments `%'...'`, the escape `%n` (the next n characters are
  plain text), and brackets `%(...)` (plain text).
- **Include files:** `$INCLUDE` and `$INCDIR` in MPL's output are acted
  on at once, so a file name can come from MPL (`$INCLUDE (%FILE)`), and
  macros defined in an include file can be used after it.

## Numbers and expressions

`%EVAL` and `%SET` compute with 32-bit signed numbers. Operators, from the
tightest binding:

1. `NOT`, `HIGH`, `LOW`, unary `-` and `+`
2. `*`, `/`, `MOD`
3. `+`, `-`
4. `SHL`, `SHR`
5. `EQ`, `NE`, `LT`, `LE`, `GT`, `GE` (true is 1)
6. `AND`, `OR`, `XOR`

`NOT` is logical: `NOT 0` is 1 and `NOT 5` is 0. Numbers may end in `H`,
`O`/`Q`, `B` or `D`. A string of one or two characters (`'A'`, `'AB'`)
is a number. MPL knows only its own `%SET` names, not assembler symbols.

The result is written as Keil writes it. From 0 to 0FFFFH it is in
hexadecimal: two digits up to 0FFH and four above, with a `0` in front
when the first digit is A to F, and an `H` (`05H`, `0BCH`, `012CH`,
`0A000H`). Other values are in decimal (`-1`, `65536`). `%LEN` and the string comparisons give plain
decimal (`5`).

Some Keil quirks that openc166 copies:

- A missing operand (`%EVAL(1 +)`, `%EVAL(())`) makes the whole value 0
  without an error.
- Shift counts are taken modulo 32: `1 SHL 33` is 2.

## Errors

The error codes are Keil's (A150 to A172). As in Keil, **MPL stops at its
first error**: the rest of the source goes to the assembler as it is
written, so later `%` signs give A6 there. Fix the first error first.

openc166 adds these messages of its own:

| Code | Kind | Meaning |
|---|---|---|
| OC33 | error | a macro or `%WHILE` that never stops: calls nested more than 256 deep, or more than 20 million calls and loop rounds; the assembly stops |
| OC34 | warning | `%IN` or `%OUT`, which use the console in Keil: `%IN` gives nothing, and the text of `%OUT` is shown in the warning |
| OC25 | error | MPL produced more than 1,000,000 lines; the assembly stops |
| OC7 | warning | under MPL, a `$INCDIR` that is not the first control on its line, which Keil ignores |

## Differences from Keil

openc166 was checked against Keil A166 V5.38 line by line, including the
text MPL produces. The known differences are all in broken calls that
real code does not contain, or in error messages:

- `%LEN %N`, where the `(` is missing and a macro call follows: Keil
  expands `N` and drops the first character of its text; openc166 drops
  the `%`, so the name `N` is left as plain text.
- `%EVAL(% (1))`: both report A166, but Keil leaves one `)` more in the
  output.
- `%EVAL((1)` with no closing parenthesis up to the end of the file: Keil
  reports A165, openc166 A153.
- `%%`: Keil crashes; openc166 reports A151.
- An MPL error is reported at the line in the source file; Keil's line
  number can be off when a call has joined lines.
- Endless macros and loops: Keil runs until it is stopped; openc166 stops
  with OC33 or OC25.
