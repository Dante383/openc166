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
openc166-sim serve image.bin --board 5wy19 --boot-mode --adc 1=0x2A0 --port 5555
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
| 4 | Bootloader boots | Done, see "K-line and gkflasher" below. ASC0, CAPCOM T0/T1/T7/T8 and the ADC are modelled; gkbus talks KWP2000 to the stock boot block through `openc166-sim serve`. gkflasher's automatic identification is refused by the boot block at gkflasher's security level, as it would be on a real ECU in boot mode. |
| 5 | Debug core and DAP | Done: breakpoints, watchpoints, stepping, reverse execution, DAP, Ghidra symbols. Keil-syntax conditions are not done (conditions are Python). |
| 6 | Flash model and CAN | Not started; the flash is a read-only image and the CAN controllers are logged storage. |
| 7 | Application runs | Mostly done. With the CAPCOM unit and the SSC stub the stock application runs its scheduler, services the watchdog and answers KWP2000 on the K-line; gkflasher's identification code finds "SIMK43 2.0 4mbit" through `openc166-sim serve`. GPT1/GPT2, PWM, CAN and the ports are still logged storage. |
| 8 | Hardware snapshot | Not started. |

## K-line and gkflasher

`openc166-sim serve` runs the ECU and serves ASC0 at `rfc2217://localhost:5555`
(pyserial's network serial port; `--raw` gives a plain `socket://` stream
instead). gkbus takes that URL as a serial K-line port, unchanged, and so
does a gkflasher built on it. (Tested with gkbus's `SerialKLineDevice` and
gkflasher's own security-access and identification code; not with the
gkflasher command line, whose checkout here predates the gkbus device API.)

```
openc166-sim serve ca663057.bin --board 5wy19 --boot-mode --adc 1=0x2A0
gkflasher -p kline -i rfc2217://localhost:5555 ...
```

The bridge acts as a K-line interface: the tester's bytes come back to it
as the bus echo, the ECU hears its own transmission, and a break from the
tester is a fast init. pyserial's RFC 2217 client waits for an
acknowledgement of break on and off, which stretches the 25 ms low and the
gap after it to about 75 ms each, too long for the 5WY19; so a break
shorter than 150 ms is replayed as the standard pattern (25 ms low, 25 ms
high, then the tester's next bytes). Simulated time is paced to real time
(a Release build of the library runs the 5WY19 at about 1.3x real time; a
Debug build is too slow for a live tester). Bytes the ECU sends at a baud
rate other than the tester's are dropped.

`--boot-mode` erases the application signature at 9FFF0H in the loaded
image, so `BL_validate_app_launch_or_download` (088000H) runs the boot
block's download loop instead of the application. `--adc 1=...` matters:
the loop resets the CPU while A/D channel 1 (the supply, presumably) reads
below 0C4H.

What the stock boot block does on the K-line, as the simulator shows it:

- It listens at 250 baud and takes the byte 0E0H as the wake-up: the 25 ms
  low of a fast init is a start bit and five zero bits at that rate. Then it
  switches to 10400 baud and expects StartCommunication within about 24 ms.
- It checks its own echo byte by byte while sending.
- It times the P2 delay before a response by sending a dummy byte at about
  250 kbaud (`S0BG = 2`), which a tester at 10400 baud does not receive.
- StartCommunication answers `C1 EF 8F`; StartDiagnosticSession 85H,
  AccessTimingParameters, ReadEcuIdentification (80H gives the
  `KR77025242--KR77025161--G7DCMQ1A...` block, 8CH the boot block version)
  and TesterPresent work.
- SecurityAccess answers 37H (time delay) for about 10 s after power-on.
  Level 01H uses the key gkflasher computes (for this image the seed 6A79H
  gives D9D7H, which the boot block compares with a copy in flash); level
  0FDH compares with a 4-byte key stored after its seed at 3F60H.
- ReadMemoryByAddress of flash needs level 0FDH: at level 01H, which
  gkflasher uses, it answers 33H. gkflasher's automatic identification
  (reading 4 bytes at 90040H, expecting `ca66`) therefore fails in boot
  mode and it offers its manual ECU list. At level 0FDH the same read
  returns `ca663057`.

### The application

With the application signature intact, the reset path calls the
application, which:

- runs its task scheduler from CAPCOM channel CC26 in compare mode 1 on
  T8 (fCPU / 128): the CC26 vector (0E8H) jumps to 0AB7B0H, which sets the
  main loop's flag (0FD64H.4) every fifth call;
- enables compare interrupts on CC4, CC11, CC19, CC21, CC26 and capture
  inputs on CC10, CC13, CC30 (crank and cam, presumably: nothing drives
  them yet), T1 and T8 overflows, ADC, SSC, PWM and CAN2 (XP1);
- talks to its driver chips on the SSC, about 600 words a second, with
  the chip select on P6.7;
- answers on the K-line like the boot block (its ReadEcuIdentification
  block lacks the VIN part), and at SecurityAccess level 01H, with
  gkflasher's key, lets ReadMemoryByAddress read 90040H: `ca663057`, which
  gkflasher's table takes for "SIMK43 2.0 4mbit".

## Boards

`board=` takes:

- `5wy19`: C167CS-LM at 24 MHz (the firmware's baud-rate code divides
  24000000; the design doc said 25); flash on CS0 seen at 0 and 80000H; RAM in
  the CS2 window; the 74HC574 latch in the CS3 window at 0A000H; XRAM1
  (0E000H, 2 KB) and XRAM2 (0C000H, 6 KB) and CAN1/CAN2 (0EF00H/0EE00H,
  logged storage) through XPERCON.
- `c167cs`: the same CPU with the image as ROM on CS0.
- `c167`, `c166`: a plain core with the image as ROM on CS0 (2 KB IRAM).

Board definitions are C++ (`src/sim/boards.cpp`). Devices written in
Python go on a chip select with `Simulator.attach()`; they are not part of
snapshots, so reverse execution does not replay them.

### Findings on the 5WY19

- External RAM on CS2. The startup code moves the CS2 window to 90000H
  for a moment (`ADDRSEL2 = 0904H`, 64 KB), runs a data bus and march test
  over 91000H..93FFFH, and moves it back to 4 KB at 0FFF000H. Without RAM
  there the test sets bit 14 of its error word; with RAM, every test passes.
  So the board very likely has an external RAM chip, which the OpenGK parts
  list does not show. The application later maps CS2 at 50000H for a
  while (`ADDRSEL2 = 0504H`). In normal running 90000H..9FFFFH is flash
  (the calibration), as the Ghidra project maps it. The chip size is
  unknown; the model has 64 KB.
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

## Peripherals

- **ASC0** in its asynchronous modes: frame timing from `S0BG` and
  `S0BRS`, the transmit buffer and shift register (`S0TBIR`, `S0TIR`),
  receive (`S0RIR`) with overrun and framing errors, loopback. Input from
  the host is a byte stream or a low pulse of a given length (a break),
  which the UART frames at its current baud rate when it samples the stop
  bit. Parity is not checked.
- **CAPCOM**: timers T0, T1, T7, T8 in timer mode (`T01CON`, `T78CON`),
  with reload and overflow interrupts (counter mode holds the timer), and
  the channels CC0..CC31 (`CCM0`..`CCM7`). Compare modes 0..3 interrupt
  at exact match times (mode 1 toggles the channel output, mode 3 sets it
  and the overflow clears it; modes 2 and 3 match once per period).
  Capture modes latch the timer on an input edge from a script
  (`capcom_edge`). Double-register compare mode is not modelled.
- **SSC** as SPI master: transfers timed from `SSCBR`, `SSCTIR` and
  `SSCRIR`, the reply from a script (`on_ssc(reply)`, given the word sent
  and port P6 for the chip selects) or 0000H.
- **A/D converter**: single, continuous and scan conversions of 10-bit
  values set from scripts (`set_adc`), 250 cycles each.
- Not yet: GPT1/GPT2, PWM, CAN (logged storage), ports (logged storage),
  the flash command set.

Host input to ASC0 is logged with its arrival cycle, so reverse execution
replays it exactly. Loading a snapshot by hand, or sending new input after
going back, starts a new timeline: the input recorded after that point is
dropped.

## Reverse execution

The debugger keeps a snapshot every `interval` instructions (20000 by
default, `set_history()`), thinning out the oldest beyond `limit`.
Reverse step restores the snapshot before the target and runs forward;
reverse continue replays each segment backwards looking for the last
breakpoint or watchpoint hit. Changing state by hand (registers, memory)
clears the history, because a replay would not repeat the change.
