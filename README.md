# openc166

An open-source assembler for the Infineon C166 and C167 microcontroller
family (including ST10 parts), compatible with the Keil A166 assembler.

It turns Keil-style assembly source into a **raw binary image** at a fixed
address. That suits small programs loaded into RAM or flash and started
directly, such as ECU payloads, bootstrap loaders, and test code. No linker
or object-file converter is needed.

openc166 runs on Linux, Windows and macOS. It needs only a C++20 compiler and
CMake.

## Status

Early development (version 0.0.x). What works today:

- **The complete C166 and C167 instruction set**, with all addressing modes.
  Every instruction form is checked byte for byte against Keil A166 V5.38:
  over 4,000 checks of code bytes, addresses and symbol values, all
  identical.
- Keil expression syntax: operators, `HIGH`/`LOW`, `SOF`/`SEG`/`PAG`/`POF`,
  character constants, bit addresses such as `PSW.3` or `0FD00H.5`, and
  extended-SFR checks inside `EXTR` sequences.
- Absolute sections (`SECTION CODE AT 0E000H`), `PROC`/`ENDP`, `ORG`, `EVEN`,
  `EQU`/`SET`/`BIT`, `DB`/`DW`/`DD`, and `DS`/`DSB`/`DSW`/`DSD`, including
  Keil's automatic alignment of word data.
- `DEFR`/`DEFA`/`DEFB`, the register-definition directives that register
  files use, with Keil's rules for repeated definitions.
- A built-in `REG167.INC` register file, written from Infineon's manuals, so
  `$INCLUDE(REG167.INC)` works without Keil installed.
- Keil's rules for label arithmetic, which decide between short and long
  instruction forms.
- Generic `JMP` and `CALL`: like Keil, openc166 picks the shortest
  instruction that reaches the target and shortens forward jumps over
  several passes. Label types (`NEAR`, `FAR`, `WORD`, ...) and the `SHORT`
  and `FAR PTR` operators work as in Keil.
- `$INCLUDE` and `$INCDIR`, with Keil's search order and listing format.
- Conditional assembly: `$SET`/`$RESET`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF`
  and the `IF`/`ELSEIF`/`ELSE`/`ENDIF` statements, with Keil's quirks (only
  bit 0 of a condition counts) and listing format.
- `LIT` text replacement (`PORT LIT 'R2'`), with Keil's rules for where
  names are replaced.
- The `$MOD167` and `$CASE` controls, with Keil's rule that these come
  before the first statement. Listing-only controls are accepted and
  ignored.
- Keil error and warning codes (A74, A111, ...), so messages line up with
  the Keil manual.

Not done yet:

- macros
- relocatable sections and linking, and OMF-166 object files
- the TASKING assembler dialect

See [docs/compatibility.md](docs/compatibility.md) for exact behaviour and
the few places where openc166 deliberately differs from Keil.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

The program is `build/cli/openc166`. On Windows with Visual Studio it is
under `build\cli\Debug\` or `build\cli\Release\`.

## Usage

```sh
openc166 payload.a66                       # writes payload.bin
openc166 payload.a66 -o out.bin -l out.lst # choose names, add a listing
```

The binary starts at the lowest address used. For example, code in
`SECTION CODE AT 0E000H` produces a file whose first byte belongs at
`0E000H`. To control this:

| Option | Meaning |
|---|---|
| `--base ADDR` | address of the first byte of the file |
| `--origin ADDR` | address of code outside sections and of sections without `AT` |
| `--fill BYTE` | value of gaps between pieces of code (default `0FFh`) |
| `--cpu c166\|c167` | instruction set (`$MOD167` in the source also selects C167) |
| `-I DIR` | also search `DIR` for `$INCLUDE` files (may be repeated) |

The exit status is 0 on success, 1 on assembly errors (no binary is
written), and 2 for usage or file errors. Messages use the format
`file:line: error A74: ...`.

[examples/payload.a66](examples/payload.a66) is a short, commented example.

## Testing

- `ctest --test-dir build` runs the unit tests and assembles the example.
- `tests/diff/` contains the differential test against Keil A166. Keil is
  **not** included. If you have your own copy, run:

  ```sh
  KEIL_A166=/path/to/A166.EXE python3 tests/diff/keil_diff.py tests/diff/cases/*.a66
  ```

  It compares every line: address, code bytes, warnings and errors, and
  the final value of every symbol. Add `--image` to also link Keil's output
  with L166 and OH166 and compare the complete memory image, and `--codes`
  to list lines where both assemblers report an error but with different
  codes. Lines marked `; DIVERGES:` are deliberate differences (see
  [docs/compatibility.md](docs/compatibility.md)) and are skipped.

  On Linux and macOS, Keil runs under Wine. To generate test lines that
  cover the whole instruction set, run
  `tests/diff/gen_sweep.py build/generated/openc166/isa.json c167 > sweep.a66`.

  The test inputs were written independently for this project. Keil's output
  is used only for the local comparison and is never stored in the
  repository.

## License

MIT; see [LICENSE](LICENSE). The bundled [doctest](third_party/doctest)
test framework is also MIT.

openc166 is not affiliated with Arm/Keil or Infineon. "Keil", "A166",
"C166", "C167" and "TASKING" are trademarks of their respective owners. The
names are used only to describe compatibility.
