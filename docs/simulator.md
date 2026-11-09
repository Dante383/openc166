# Simulator and debugger

The plan is in [debugger-design.md](debugger-design.md). This page says how
to use what exists and where it stands.

## Quick start

```python
from openc166 import sim

s = sim.Simulator.from_file("ca663057.bin", board="5wy19")
s.symbols.load_ghidra("ca663057.symbols.json")   # optional
s.break_at(0x882D0)
print(s.cont())                # Stop(reason='breakpoint', id=1)
print(s.where(), hex(s.r[15]), s.sfr["SYSCON"], s.flag("Z"))
s.watch("PSW.Z", "change")     # bits, SFRs, symbols or addresses
s.step(); s.step_over(); s.step_out()
s.reverse_step()               # deterministic re-run from a snapshot
s.watch(0xE79E, "write"); s.reverse_continue()   # who last wrote 0E79EH?
for i in s.disassemble(count=5): print(hex(i.address), i.text)
snap = s.save(); s.cont(100000); s.load(snap)
```

Your own code, at source level:

```python
import openc166
from openc166 import sim

r = openc166.assemble_file("payload.a66")
s = sim.Simulator.from_result(r, board="c167")
s.break_at("payload.a66:42", condition=lambda m: m.r[4] > 3)
```

Command line:

```
openc166-sim disasm image.bin --base 0x80000 --start 0x88046 --count 40
openc166-sim disasm image.bin --base 0x80000 --source > image.a66   # reassembles byte for byte
openc166-sim run image.bin --board 5wy19 --instructions 5000000 --freeze-watchdog
openc166 -g payload.json payload.a66      # debug information for the debugger
```

`openc166-sim run` ends with the list of registers and addresses the
firmware touched that nothing models yet, grouped by register: that list
is the modelling to-do (design §3.5).

Editors: `python -m openc166.dap` is a Debug Adapter Protocol server;
[editors/vscode](../editors/vscode/README.md) registers it with VS Code
and shows a launch configuration. Step back and reverse continue work
from the toolbar.

Ghidra symbols: run `tools/ghidra/ExportOpenc166Symbols.py` in Ghidra's
Script Manager; load the JSON with `Symbols.load_ghidra()` or the
`ghidraSymbols` launch argument. A CSV export of the Symbol Table window
also works.

## Status by milestone

| # | Milestone | State |
|---|---|---|
| 1 | Debug information | Done. Every byte of the examples maps to a line (python test). |
| 2 | Disassembler | Done. The whole 512 KB stock dump reassembles byte for byte; one data word at 20010H decodes as a `JMPR` that wraps inside its segment, which A166 cannot spell, and stays `DB`. |
| 3 | CPU core, bus, snapshots | Done except the hardware oracle (below). The stock reset path passes all its self-tests: error word 0000H. |
| 4 | Bootloader boots | Not started: needs ASC0, a timer and the ADC stub. |
| 5 | Debug core and DAP | Done: breakpoints, watchpoints, stepping, reverse execution, DAP, Ghidra symbols. Keil-syntax conditions are not done (conditions are Python). |
| 6 | Flash model and CAN | Not started; the flash is a read-only image and the CAN controllers are logged storage. |
| 7 | Application runs | Not started. The application waits for CAPCOM timer T1 at 0A3EFAH. |
| 8 | Hardware snapshot | Not started. |

## Boards

`board=` takes:

- `5wy19`: C167CS-LM at 25 MHz; flash on CS0 seen at 0 and 80000H; RAM in
  the CS2 window; the 74HC574 latch in the CS3 window at 0A000H; XRAM1
  (0E000H, 2 KB) and XRAM2 (0C000H, 6 KB) and CAN1/CAN2 (0EF00H/0EE00H,
  logged storage) through XPERCON.
- `c167cs`: the same CPU with the image as ROM on CS0.
- `c167`, `c166`: a plain core with the image as ROM on CS0 (2 KB IRAM).

Board definitions are C++ (`src/sim/boards.cpp`). Devices written in
Python go on a chip select with `Simulator.attach()`; they are not part of
snapshots, so reverse execution does not replay them.

### Findings on the 5WY19

- The startup code moves the CS2 window to 90000H (`ADDRSEL2 = 0904H`, 64
  KB) and runs a data bus and march test over 91000H..93FFFH. Without RAM
  there it sets bit 14 of its error word; with RAM, every test passes. So
  the board very likely has external RAM, which the OpenGK parts list does
  not show, and 90000H..9FFFFH is RAM at run time, not flash (the Ghidra
  project maps it as flash "Calibration"). The chip size is unknown; the
  model fills the window.
- `ADDRSEL1 = 1008H` (1 MB at 100000H) and `ADDRSEL4 = 0FFE0H` (4 KB at
  0FFE000H) are programmed but no access to them has been seen yet.

## What the core models, and how closely

- The whole instruction set, decoded from `src/isa/c166.tbl`. Flags follow
  the instruction set manual; they are checked by unit tests, not yet
  against a real C167 (design §6: the MiniMon oracle is the next step).
- Register banks in IRAM through CP; DPP paging; EXTR, EXTP(R), EXTS(R)
  and ATOMIC, which also block interrupts. The debugger steps a sequence as
  one unit.
- System stack with STKOV/STKUN traps. The circular stack of
  SYSCON.STKSZ is **not** modelled: SP moves freely.
- Hardware traps (NMI, stack, UNDOPC, PRTFLT, ILLOPA, ILLINA) with TFR; a
  class B trap aborts the instruction, but side effects already done stay.
- Interrupts by ILVL and GLVL, with PEC for levels 14 and 15. The interrupt
  table is the C167's (`c167_interrupts()`); the ESFR IC addresses of
  CC16..CC31, T7, T8, PWM and XP0..XP3 are from memory of the data sheet
  and should be checked against the firmware's vector use.
- SRST, EINIT, IDLE, PWRDN, SRVWDT, DISWDT. Watchdog at fCPU/2 or /128 with
  WDTREL, and reset flags: power-on sets LHWR, SHWR, SWR; watchdog WDTR,
  SHWR, SWR; SRST SWR. EINIT clears SWR, SHWR, LHWR; SRVWDT clears WDTR.
  SYSCON's configuration bits lock at EINIT.
- Chip selects from ADDRSELx/BUSCONx (BUSACT), with ADDRSEL2 and 4 taking
  priority over 1 and 3.
- Time: 2 cycles per instruction word, +2 for a taken branch, MUL 10, DIV
  20. No wait states. Peripherals schedule events on this clock; nothing
  reads host time, so runs are reproducible.
- Every SFR without a model is plain storage, and its first read and first
  write are logged with the PC. `stop_on_unmodelled` stops there.

## Reverse execution

The debugger keeps a snapshot every `interval` instructions (20000 by
default, `set_history()`), thinning out the oldest beyond `limit`.
Reverse step restores the snapshot before the target and runs forward;
reverse continue replays each segment backwards looking for the last
breakpoint or watchpoint hit. Changing state by hand (registers, memory)
clears the history, because a replay would not repeat the change.
