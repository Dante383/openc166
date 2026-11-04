# openc166

An open-source assembler for the Infineon C166 and C167 microcontroller
family (including ST10 parts), compatible with the Keil A166 assembler.

It turns Keil-style assembly source into a **raw binary image** at a fixed
address, or into an Intel HEX or Motorola S-record file. That suits small
programs loaded into RAM or flash and started directly, such as ECU
payloads, bootstrap loaders, and test code. No linker or object-file
converter is needed.

openc166 runs on Linux, Windows and macOS. It needs only a C++20 compiler and
CMake.

## Status

Early development (version 0.x). What works today:

- **The complete C166 and C167 instruction set**, with all addressing modes.
  Every instruction form is checked byte for byte against Keil A166 V5.38:
  over 5,000 checks of code bytes, addresses and symbol values, all
  identical.
- Keil expression syntax: operators, `HIGH`/`LOW`, `SOF`/`SEG`/`PAG`/`POF`,
  `BOF`, `DATA3`/`DATA4`/`DATA8`/`DATA16`, character constants, bit
  addresses such as `PSW.3` or `0FD00H.5`, and extended-SFR checks inside
  `EXTR` sequences.
- Sections with an address (`SECTION CODE AT 0E000H`) and without one.
  openc166 places sections without `AT` as Keil's L166 linker would, or at
  addresses you give (`--section NAME=ADDR`), so code that leaves placement
  to the linker can still be assembled straight to a binary. `DGROUP` and
  `CGROUP` keep sections in one data page or code segment, as in L166.
  `BIT` sections and `DBIT` hold bit variables, placed bit by bit as L166
  places them.
- `PROC`/`ENDP`, `ORG`, `EVEN`, `EQU`/`SET`/`BIT`, `DB`/`DW`/`DD`,
  `DS`/`DSB`/`DSW`/`DSD`, and `DPPTR`/`DSPTR`/`DBPTR`, including Keil's
  automatic alignment of word data.
- Segmented mode (`$SEGMENTED`): `ASSUME`, `DPPn:` prefixes, and Keil's
  rules for which data page register an address goes through.
- `DEFR`/`DEFA`/`DEFB`, the register-definition directives that register
  files use, with Keil's rules for repeated definitions.
- A built-in `REG167.INC` register file, written from Infineon's manuals, so
  `$INCLUDE(REG167.INC)` works without Keil installed.
- Keil's rules for label arithmetic, which decide between short and long
  instruction forms.
- Generic `JMP` and `CALL`: like Keil, openc166 picks the shortest
  instruction that reaches the target and shortens forward jumps over
  several passes. Label types (`NEAR`, `FAR`, `WORD`, ...) and the `SHORT`
  and type operators (`FAR x`, `WORD PTR x`, ...) work as in Keil.
- `$INCLUDE` and `$INCDIR`, with Keil's search order and listing format.
- Conditional assembly: `$SET`/`$RESET`, `$IF`/`$ELSEIF`/`$ELSE`/`$ENDIF`
  and the `IF`/`ELSEIF`/`ELSE`/`ENDIF` statements, with Keil's quirks (only
  bit 0 of a condition counts) and listing format.
- `LIT` text replacement (`PORT LIT 'R2'`), with Keil's rules for where
  names are replaced.
- The `$MOD167`, `$CASE` and `$SEGMENTED` controls, with Keil's rule that
  these come before the first statement. Listing-only controls are accepted
  and ignored.
- Keil error and warning codes (A74, A111, ...), so messages line up with
  the Keil manual.
- Raw binary, Intel HEX and Motorola S-record output. The HEX records match
  Keil OH166's `H167` format.
- Several source files linked into one program, as L166 does: `EXTERN`,
  `PUBLIC` and `GLOBAL` symbols, and sections of the same name combined
  across files. Each file is assembled separately (a "module"), so a name
  that is not `PUBLIC` stays private to its file.
- A Python package and a C API, for tools that assemble code themselves.

Not done yet:

- macros
- OMF-166 object files and libraries
- the TASKING assembler dialect

See [docs/compatibility.md](https://github.com/Dante383/openc166/blob/main/docs/compatibility.md) for exact behaviour and
the few places where openc166 deliberately differs from Keil.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

The program is `build/cli/openc166`. On Windows with Visual Studio it is
under `build\cli\Debug\` or `build\cli\Release\`. The shared library with
the C API is built in `build/src/`.

## Usage

```sh
openc166 payload.a66                       # writes payload.bin
openc166 payload.a66 -o out.bin -l out.lst # choose names, add a listing
openc166 payload.a66 -o payload.hex        # Intel HEX
openc166 payload.a66 -o payload.s19        # S-records (S1)
openc166 main.a66 lib.a66 -o payload.bin   # link two source files
```

Several source files are linked into one program, the way Keil's L166
linker joins the object files of A166:

- An `EXTERN` name in one file takes the value of the `PUBLIC` or `GLOBAL`
  name in another. A name that no file exports is an error.
- Sections with the same name and class are joined into one: the parts of
  a `PUBLIC` or `GLOBAL` section follow one another, the parts of a
  `COMMON` section lie on top of each other.
- Without `-o`, the output is named after the first file. With `-l`, the
  listing has one part per file and a symbol table that names the file
  each symbol comes from.
- Code outside sections starts at `--origin` in every file, so normally
  each file puts its code in sections.

`$INCLUDE` looks in each file's own directory.
[docs/compatibility.md](https://github.com/Dante383/openc166/blob/main/docs/compatibility.md#linking-several-files)
has the exact rules.

The output format follows the extension of the `-o` file: `.hex`, `.h86`
and `.ihx` give Intel HEX; `.srec`, `.s19`, `.s28`, `.s37`, `.mot` and
`.sx` give S-records; anything else gives a raw binary. `--format` chooses
it explicitly.

A raw binary starts at the lowest address used. For example, code in
`SECTION CODE AT 0E000H` produces a file whose first byte belongs at
`0E000H`. Gaps between pieces of code are filled. HEX and S-record files
carry the address of every record, so they hold only the bytes used.

| Option | Meaning |
|---|---|
| `-f, --format FMT` | `bin`, `hex` or `srec` (default: from the output extension) |
| `--origin ADDR` | address of code outside sections, and the lowest address for sections without `AT` (default 0) |
| `--section NAME=ADDR` | put section `NAME`, which has no `AT`, at `ADDR` (may be repeated) |
| `--base ADDR` | binary: address of the first byte of the file |
| `--fill BYTE` | binary: value of gaps between pieces of code (default `0FFh`) |
| `--record-size N` | HEX and S-records: data bytes per record (default 16) |
| `--cpu c166\|c167` | instruction set (`$MOD167` in the source also selects C167) |
| `-I DIR` | also search `DIR` for `$INCLUDE` files (may be repeated) |

Sections without `AT` go in order of first appearance to the first free,
suitably aligned place from the origin on, as L166 places them by default.
`--section` overrides that for chosen sections, like L166's `SECTIONS`
control. The listing ends with a table of where each section went.
[examples/sections.a66](https://github.com/Dante383/openc166/blob/main/examples/sections.a66) shows both.

Intel HEX files use extended linear address records (type 04) for addresses
above 64 KB, like OH166 with its `H167` option. S-records use S1, S2 or S3
records by extension (`.s19`, `.s28`, `.s37`); otherwise the smallest kind
that holds every address.

The exit status is 0 on success, 1 on assembly errors (no output is
written), and 2 for usage or file errors. Messages use the format
`file:line:column: error A74: ...`.

[examples/payload.a66](https://github.com/Dante383/openc166/blob/main/examples/payload.a66) is a short, commented example.
[examples/mtos/](https://github.com/Dante383/openc166/tree/main/examples/mtos) holds real ECU payloads for Siemens SIMK43
ECUs, taken from gkflasher.

## Python

```sh
pip install openc166     # or "pip install ." in a checkout
```

Wheels are published for Linux, Windows and macOS on x86 and ARM; other
systems build from source, which needs CMake and a C++20 compiler. To get
just the `openc166` command, `pipx install openc166` works too.

```python
import openc166

result = openc166.assemble("""
$MOD167
PROG    SECTION CODE AT 0E000H
        MOV     R4, #0BEEFH
        RET
PROG    ENDS
        END
""")
image = result.image()     # raises openc166.AssemblyError if there are errors
image.base                 # 0xE000, the address of image.data[0]
image.data                 # b'\xe6\xf4\xef\xbe\xcb\x00'
```

- `openc166.assemble(source, *, file_name=None, cpu=None, origin=0, include_dirs=(), sections=None)`
  assembles a `str` or `bytes`. `sections` gives addresses for sections
  without `AT`, e.g. `{"VARS": 0xF600}`. `openc166.assemble_file(path, ...)`
  reads a file; `$INCLUDE` then also searches the file's directory.
- The result has `ok`, `diagnostics` (each with `severity`, `code`,
  `message`, `file`, `line`, `column`; `str()` gives the usual one-line
  message), `errors`, `warnings`, `chunks` (address and bytes of each piece
  of code), `symbols` (a dict of name to `Symbol`), `symbol(name)` (lookup
  ignoring case), `sections` (a dict of name to `Section`: `type`, `align`,
  `address`, `size`, and `placement`, which is `"at"`, `"given"` or
  `"located"`), `listing` and `cpu`.
- `openc166.assemble_many({"main.a66": text1, "lib.a66": text2}, ...)`
  links several sources into one program, like several files on the
  command line; `openc166.assemble_files([path1, path2], ...)` reads them
  from files. Each `Symbol` then says which file defines it (`module`) and
  whether it is `PUBLIC` or `GLOBAL` (`exported`). `result.modules` lists
  the file names; `result.symbols` holds one symbol per name (the exported
  one if there is one), `result.all_symbols` all of them. Each `Section`
  has its `class_name` and `group`.
- `result.image(base=None, fill=0xFF)` lays all code out as one block, like
  the command-line tool.
- `result.intel_hex(record_size=16)` and
  `result.srec(record_size=16, address_size=None)` return Intel HEX and
  S-record text (`address_size` 2, 3 or 4 picks S1, S2 or S3 records).

The package needs Python 3.9 or newer. It also installs the `openc166`
command. It loads the library through `ctypes`, so one wheel works with
every Python version.

## C API

[include/openc166/openc166.h](https://github.com/Dante383/openc166/blob/main/include/openc166/openc166.h) declares a plain C
interface, exported by the shared library (`libopenc166.so`,
`libopenc166.dylib` or `openc166.dll`). Any language that can call C can
use it: `oc166_assemble` assembles one source, `oc166_assemble_many`
links several. The header documents every function; the Python package is
an example of a complete binding.

## Testing

- `ctest --test-dir build` runs the unit tests, the C API test and the
  Python package tests, and assembles the examples, checking every byte.
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
  [docs/compatibility.md](https://github.com/Dante383/openc166/blob/main/docs/compatibility.md)) and are skipped.

  Given a directory instead of a file, it treats each `.a66` file in it as
  one module, links Keil's objects with L166, and compares the linked
  image and the linker's errors with openc166's:

  ```sh
  KEIL_A166=/path/to/A166.EXE python3 tests/diff/keil_diff.py tests/diff/cases/link/*/
  ```

  On Linux and macOS, Keil runs under Wine. To generate test lines that
  cover the whole instruction set, run
  `tests/diff/gen_sweep.py build/generated/openc166/isa.json c167 > sweep.a66`.

  The test inputs were written independently for this project. Keil's output
  is used only for the local comparison and is never stored in the
  repository.

## License

MIT; see [LICENSE](https://github.com/Dante383/openc166/blob/main/LICENSE). The bundled [doctest](https://github.com/Dante383/openc166/tree/main/third_party/doctest)
test framework is also MIT.

openc166 is not affiliated with Arm/Keil or Infineon. "Keil", "A166",
"C166", "C167" and "TASKING" are trademarks of their respective owners. The
names are used only to describe compatibility.
