# openc166 — Simulator and Debugger: Design

Status: written 2026-11-09 as the design. Milestones 1-5 and most of 7 are
implemented; docs/simulator.md says what exists and how closely it is
modelled.

Scope: a C166/C167 simulator and a source- and disassembly-level debugger on
top of it, as the next part of the openc166 toolchain. The first target is the
Siemens 5WY19 engine ECU (C167CS-LM), running both its stock firmware and
firmware assembled with openc166.

---

## 1. Goals and non-goals

**Goals**

- Step through stock ECU firmware (no source, symbols from Ghidra) and through
  our own firmware (source level, including macros and include files).
- A simulator complete enough that the stock firmware boots, reaches its main
  loop and answers diagnostics over K-line and CAN.
- Modern debugging: reverse execution, watchpoints on memory, SFRs and single
  bits, scripted conditions, and a standard protocol so existing editors work.
- Scriptable from Python, so the simulator also serves as a test bench for
  payloads and for gkflasher.

**Non-goals, for now**

- **Breakpoints and stepping on the real CPU.** The classic C167 has no
  on-chip debug hardware, and code in flash cannot be patched with traps.
  Dropped to keep the design small. The only planned hardware link is reading
  memory from a real ECU to seed a simulator session (§9).
- A running engine. Sensors may read as faulty and fault codes may be stored.
- Electrical or bit-level accuracy of buses. CAN and serial are modelled as
  whole frames and bytes.
- A custom GUI.

---

## 2. Architecture

```
  VS Code / Neovim / ...        Python scripts, pytest        gkflasher
          | DAP                         | API                  | pty / SocketCAN
  +-------+-----------------------------+------+               |
  |               debug core                   |               |
  |  breakpoints, stepping, symbols, lines,    |               |
  |  expressions, snapshots, reverse execution |               |
  +----------------------+---------------------+               |
                         |                                     |
  +----------------------+---------------------+               |
  |                 machine                    |               |
  |  CPU core | bus and chip selects | clock   |               |
  |  on-chip peripherals (C++)                 +---------------+
  |  board devices (C++ or Python)             |  host bridges
  +--------------------------------------------+
```

- **Machine**: CPU core, memory map and peripherals. A C++ library with a C
  API and a Python binding, in the same style as the assembler.
- **Debug core**: everything a debugger needs that is not specific to a
  frontend. Talks only to the machine interface.
- **Frontends**: a DAP server (Debug Adapter Protocol) and the Python API.
- **Board definition**: one file per ECU that says which CPU variant, which
  memory images, which external devices and which host bridges.

---

## 3. Components

### 3.1 Debug information from the assembler

The assembler today writes images, a listing and a symbol table. The debugger
needs a machine-readable map:

- address range ↔ source file and line, with the include stack and, for
  macro-generated code, both the call site and the line inside the macro;
- symbols with their type (`NEAR`, `FAR`, `WORD`, `BIT`, ...), module and
  section;
- section placement.

Format: a JSON sidecar next to the image first, because our own tools are the
only consumers. ELF with DWARF line tables can follow if other tools need it.

### 3.2 Disassembler

Generated from `src/isa/c166.tbl` (through the `isa.json` that `isagen`
already produces), so the encoder and decoder cannot disagree. Round-trip test:
disassemble, reassemble with openc166, compare bytes. Output uses SFR and bit
names from the register file and symbols from the debug information.

### 3.3 CPU core

- The full C166/C167 instruction set. The table gives decoding; the semantics
  (about 84 mnemonics) are written by hand.
- Register banks through `CP`, with the general registers living in memory as
  on the real chip.
- `DPP0-3` paging, `CSP:IP`, the system stack with `STKOV`/`STKUN` traps.
- `EXTR`/`EXTP`/`EXTS`/`ATOMIC` sequences: interrupts are locked for the
  sequence, and the debugger steps the sequence as one unit.
- Interrupt arbitration by level and group, hardware traps and their
  priorities, and PEC transfers.
- `SRST`, `EINIT`, `IDLE`, `PWRDN`, `SRVWDT`, `DISWDT` with their real effects.

### 3.4 Bus and memory

- Internal RAM, SFR and ESFR areas, XRAM and X-peripherals enabled through
  `XPERCON`.
- External bus through `ADDRSEL1-4` and `BUSCON0-4`: each chip-select window
  maps to a device (ROM image, flash model, latch, RAM).
- Every access goes through one dispatch point that watchpoints, access
  logging and the "unmodelled register" check hook into.

### 3.5 Peripherals

On-chip peripherals are C++, because they are shared by every board and run
on the hot path. Board-level devices can be C++ or Python.

**Rule for missing pieces:** an access to an SFR or address that nothing
models is logged with the program counter and can stop execution. Modelling is
driven by what the firmware actually touches, not by the manual.

### 3.6 Time

- A virtual clock advanced by per-instruction cycle counts from the
  instruction set manual. Bus wait states are ignored at first.
- Peripherals schedule events on that clock (timer overflow, byte received,
  conversion done). Nothing reads host time, so a run is reproducible.
- Host bridges inject input as timestamped events, which are recorded so a
  session can be replayed exactly.
- The watchdog can be frozen from the debugger.

### 3.7 Snapshots and reverse execution

- A snapshot is the full machine state: CPU, memory, peripheral state and the
  event queue. RAM is small (3 KB internal, 8 KB XRAM on the C167CS), so
  snapshots are cheap; flash is copied only when written.
- Reverse step and reverse continue: restore the nearest earlier snapshot and
  re-run forward to the target point. Determinism (§3.6) makes this exact.
- "Who last wrote this address?" is reverse-continue with a write watchpoint.

### 3.8 Debug core and frontends

- Breakpoints by address, symbol or source line; conditions and actions as
  Python callables or Keil-syntax expressions (reusing `src/expr.cpp`).
- Watchpoints on memory ranges, SFRs and bits such as `PSW.3`, on read, write
  or value change.
- C166 views: register bank contexts, DPP translation of each data access,
  interrupt and trap state, stack limits, decoded SFR bitfields.
- Symbols: openc166 debug information for our code; a Ghidra export
  (functions, labels, comments) for stock firmware.
- DAP server for editors. Python API for scripts and tests.

### 3.9 Host bridges

- ASC0 ↔ a pseudo-terminal, so gkflasher's K-line code connects unmodified.
- CAN ↔ SocketCAN (`vcan`), so standard CAN tools and gkflasher's CAN code
  connect unmodified.
- Ports and ADC channels ↔ values set from a script.

---

## 4. First target: Siemens 5WY19

### 4.1 Hardware

From the
[OpenGK board page](https://opengk.org/index.php?title=Siemens_5WY19_PCB_Components):

| Part | Role | Model |
|---|---|---|
| C167CS-LM, 25 MHz (8 MHz crystal) | CPU, ROMless, XRAM, two CAN controllers | Core plus on-chip peripherals |
| AM29F400BB | 512 KB boot-block flash | ROM image first, then the command set (unlock, erase, program, ID) |
| A82C250 | CAN transceiver | None needed |
| TPIC8101 | Knock sensor interface on SPI | Stub with plausible replies |
| 74HC574 | Output latch | Memory-mapped output port (§4.3) |
| Siemens custom driver chips (ATM3x, ATM46C3) | Injector, ignition and actuator drivers | Logged outputs; diagnostic replies stubbed as "no fault" |

No external RAM chip is listed. Not confirmed against the schematic.

### 4.2 Peripheral use in the stock firmware

Surveyed in Ghidra on `ca663057_retail_virgin_immo_off.bin` by listing
references to about 45 key registers. This is a sample, not a complete sweep.

| Peripheral | Finding | Work |
|---|---|---|
| CAPCOM | About 30 channel interrupt handlers and hundreds of references: crank capture, ignition, injection and task timing | Largest block; timers T0/T1/T7/T8 and compare mode must be right |
| GPT1, GPT2 | T2, T4, T5, T6 used; T3 not referenced | Medium |
| ASC0 | K-line in the bootloader; in the application also used by the SMARTRA immobilizer code | Small; two devices share the port |
| CAN | Both controllers (`0xEE00`, `0xEF00`), bootloader and application, including CCP | Medium |
| SSC | Interrupt-driven state machine, chip select on P6.7, replies stored in diagnostic buffers | Small stub |
| ADC | Interrupt-driven in the application, polled in the bootloader | Small |
| PWM | Used | Small; logged outputs |
| Watchdog | Used; reset cause read at startup | Small; reset-cause bits must be right |
| Ports | Heavily used (P2 alone: 165 references) | Logged stubs, scripted inputs |
| RTC, `SYSCON2`, PEC channel 0 | Not referenced | Skip |

### 4.3 What the reset path requires

Read from the startup code at `0x088046`:

- The reset vector at 0 jumps to `0x088046`, so the flash appears both at
  segment 0 and at `0x80000`. Inferred from the layout, not verified against
  the schematic.
- A CPU register self-test that hangs on failure. A good first test of the
  core.
- A march test over RAM.
- `SYSCON` is compared with an expected value and a software reset is issued
  on mismatch, so `SRST` and `EINIT` must behave correctly.
- Four chip-select windows are programmed. One is a 4 KB window at `0xA000`
  that is written immediately with a shadow byte from RAM: very likely the
  74HC574 output latch.
- `XPERCON` enables both CAN controllers and both XRAM blocks.

### 4.4 Consequence for ordering

The **bootloader** uses only ASC0, one timer, polled ADC, CAN and the flash
driver. It is the first milestone. The **application** needs CAPCOM and the
timers working before its scheduler runs, so it comes second.

---

## 5. Milestones

| # | Milestone | Done when |
|---|---|---|
| 1 | Debug information from the assembler | A JSON map is written; a test maps every byte of the examples back to its source line |
| 2 | Disassembler | The stock dump's code disassembles and reassembles byte for byte |
| 3 | CPU core, bus, snapshots | Instruction tests pass (§6); the stock reset path passes its register and RAM self-tests |
| 4 | Bootloader boots | ASC0, timer, watchdog and ADC stub are in; gkflasher identifies the simulated ECU over the pty |
| 5 | Debug core and DAP | Breakpoints, stepping, memory and register views and reverse step work from VS Code, with Ghidra symbols on stock code and source lines on ours |
| 6 | Flash model and CAN | gkflasher reads and writes the simulated flash over K-line and over `vcan` |
| 7 | Application runs | Timers, CAPCOM, interrupts by priority, SSC stub; the application reaches its main loop and answers KWP2000 requests |
| 8 | Hardware snapshot | RAM and SFRs dumped from a real ECU load into a simulator session |

Milestones 1 and 2 are independent of the rest and of each other.

---

## 6. Testing

- **Instruction semantics against real hardware.** Assemble a small snippet,
  run it on a real C167 through MiniMon (the harness in
  `tests/acceptance/minimon.py` already drives one), read back registers,
  `PSW` and memory, and compare with the simulator. This gives a per-instruction
  oracle, flags included, without relying on anyone's reading of the manual.
- **Disassembler round trip** against the assembler and against Ghidra's
  listing of the stock dump.
- **Firmware self-tests**: the stock reset path checks the registers and RAM
  itself.
- **Determinism**: the same inputs give the same trace; restoring a snapshot
  and re-running reaches the same state.
- **End to end**: gkflasher sessions against the simulated ECU, in CI.

---

## 7. Risks

- **Timing.** The application's scheduler is timer-driven. Cycle counts
  without bus wait states may be off enough to matter for watchdog or protocol
  timeouts. Mitigation: per-chip-select wait states from `BUSCON` if needed.
- **CAPCOM size.** 32 channels in several modes, two timer pairs, and
  double-register compare. It is the largest single peripheral.
- **Undocumented Siemens driver chips.** If the firmware refuses to continue
  without specific diagnostic replies, those have to be worked out from the
  code.
- **Shared ASC0.** K-line and the SMARTRA immobilizer use the same port; how
  the board switches between them is not yet known.

---

## 8. Open questions

- Confirm against the schematic: flash mirroring, the device behind the
  `0xA000` window, the other three chip-select windows, absence of external
  RAM.
- How K-line and SMARTRA share ASC0.
- A complete sweep of SFR references in the stock firmware, to replace the
  sample in §4.2.
- Whether an ELF/DWARF output is wanted in addition to the JSON map.

---

## 9. Later: hardware link

Not a debugger backend. A memory read path (MiniMon or the stock diagnostic
protocol) dumps RAM and SFRs from a running ECU, and the simulator resumes
from that state. This skips modelling whatever happened before the dump and
gives reverse execution from that point on.
