# Debug information format

`openc166 -g FILE`, `oc166_result_debug_info()` and `Result.debug_info()`
write a JSON map from addresses to source, read by the simulator's debug
core (`openc166.sim.Symbols.load_debug_info`). Version 1:

```json
{
  "format": "openc166-debug",
  "version": 1,
  "cpu": "c167",
  "modules": ["main.a66"],
  "files": ["main.a66", "inc/regs.inc"],
  "sections": [
    {"name": "PROG", "type": "CODE", "align": "WORD", "address": 57344, "size": 12,
     "class": "", "group": ""}
  ],
  "symbols": [
    {"name": "START", "kind": "label", "type": "number", "value": 57344,
     "label_type": "near", "module": 0, "section": 0}
  ],
  "lines": [
    {"address": 57344, "size": 2, "module": 0, "file": 0, "line": 7, "section": 0},
    {"address": 57346, "size": 2, "module": 0, "file": 0, "line": 8, "section": 0,
     "macro": {"file": 0, "line": 3, "depth": 1}},
    {"address": 57348, "size": 4, "module": 0, "file": 1, "line": 2, "section": 0,
     "include": [[0, 12]]}
  ]
}
```

Numbers are plain JSON integers (addresses are physical, 24-bit).

**files** — every file named below, by index: the module sources, include
files and files that define macros.

**sections** — as `Result.sections`; a BIT section adds `bit` and
`bit_size`.

**symbols** — every symbol except LITs. `kind` is `label`, `equ`, `set`,
`bit` or `extern`; `type` is `number`, `gpr`, `gpr_byte`, `sfr`, `bit` or
`cc`. A `bit` symbol's `value` is the word address and `bit` its position.
`value` is `null` if it could not be worked out. Labels carry `label_type`
(`near`, `far`, `byte`, `word`, `dword`) and the index of the `section`
that holds them (-1 if none). `exported` is present and true for PUBLIC
and GLOBAL names; an `extern` has `extern_type`.

**lines** — one entry per source line that emitted bytes, in assembly
order:

- `address`, `size`: the bytes;
- `module`, `file`, `line`: where the line is (1-based). For code from a
  macro, REPT, IRP or IRPC expansion this is the call site;
- `include`: for a line in an include file, the `[file, line]` of each
  `$INCLUDE` that led to it, outermost first;
- `macro`: for expanded code, the `file` and `line` of the body line it
  came from and the expansion `depth`. A REPT, IRP or IRPC block expands at
  its ENDM, so its call site is the ENDM line.
- `section`: the section index, or -1 for code outside sections.

Lines in false IF blocks, and lines that emit no bytes, are left out. Bits
of BIT sections are not lines.
