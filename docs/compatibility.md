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
  - bit, bit-field and extended-SFR addressing.
- **Expressions:** `+ - * / MOD SHL SHR AND OR XOR NOT`, comparisons,
  `HIGH LOW SOF SEG PAG POF`, `$`, character constants, and bit selection
  (`base.n`). See [Expression details](#expression-details).
- **Directives:** `SECTION ... [AT addr]` / `ENDS`, `PROC [NEAR|FAR|INTERRUPT|TASK]`
  / `ENDP`, `LABEL`, `EQU`, `SET`, `BIT`, `DB`, `DW`, `DD`, `DS`, `DSB`, `DSW`,
  `DSD`, `ORG`, `EVEN` and `END`.
- **Accepted and ignored:** these only matter to a linker or debugger.
  - Directives: `NAME`, `PUBLIC`, `GLOBAL`, `ASSUME`, `REGDEF`, `REGBANK`,
    `SSKDEF`, `PECDEF`, `TYPEDEC`.
  - Listing and object-file controls: `$NOLIST`, `$TITLE(...)`, `$DEBUG`,
    `$SEGMENTED`, and similar.
- **Controls:** `$MOD167`, `$MOD166`/`$NOMOD166`, `$CASE`/`$NOCASE`.
  `$MODV2` falls back to the C167 set, with warning OC2.

## Not supported yet

These produce error OC3 (or OC4 for externals), so a missing feature is never
silently skipped:

- `$INCLUDE`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF` and `$SET`/`$RESET`
- `MACRO` and the other macro facilities
- `EXTERN`/`EXTRN` (a raw image has no linker to resolve them)
- `DEFR`, `DEFB`, `DEFA`, `LIT`, `DBIT`, `DGROUP`, `CGROUP`, `COMREG`, and BIT sections
- the XC16x/Super10 extended instruction set

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
| `BIT` naming a non-bit-addressable address | silently defines `0FD00H.0` | error A103 |
| `-label`, `NOT label`, `-$` | wrong value (`-L` gives `0FFF0H` instead of `0FFFEH` when `L` is 2) | error A63 |
| `-(L1 - L0)`, `NOT (L1 - L0)`, `-(L1 - 1)` | wrong value, no warning | error A63 |
| `number - label` (`10H - L1`) | wrong value | error A63 |
| decimal fraction such as `#1.0` | accepted as 0 | error A70 |
| instruction at an odd address | accepted | error A134 |
| instruction outside a `PROC` | error A45 | accepted |
| instruction or data outside any `SECTION` | error A44 | placed at `--origin` |
| error inside an instruction | sometimes still emits code | emits nothing (no binary is written anyway) |

Keil's wrong values for negated addresses also appear in the linked program,
so they are real miscompilations, not listing glitches. Keil appears to
multiply the address by 8 before negating it. `-(L1 - L0)` can be written
as `0 - (L1 - L0)`, which both assemblers compute correctly.

Keil leaves some branch targets, such as `JMPR cc_UC, $+4` or
`JMPR cc_UC, label+2`, for its linker to fill in. openc166 resolves them
directly, because the target address is known.
