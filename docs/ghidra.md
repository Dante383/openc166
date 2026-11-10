# Debugging in Ghidra

openc166 is a back-end for Ghidra's Debugger (Trace RMI, Ghidra 11 and
later): the firmware runs in the openc166 simulator and Ghidra drives it.
You get Ghidra's dynamic listing following the program counter in your
annotated program, the registers, memory, breakpoints and watchpoints set
from the listing, stepping, and stepping and running backwards.

## Setup

1. Install the C166 processor module (`c166-ghidra-module`) into Ghidra, as
   for static analysis.
2. Install openc166 into a Python 3.10 or later that has a protobuf at
   least as new as the one Ghidra's client was generated with (Ghidra 12.2:
   protobuf 6.31): `pip install openc166 protobuf`.
3. Add the launcher to Ghidra, with that Python:

   ```
   python3 -m openc166.ghidra --install-launcher /path/to/ghidra_12.2_PUBLIC
   ```

   This writes a small extension (`openc166-debugger`, holding only the
   launcher) into the user Extensions directory of that Ghidra, with the
   Python that ran the command as the launcher's default. Restart Ghidra.

## Use

Open the firmware program in the Debugger tool. In the launch menu choose
**openc166 simulator** and set:

| Option | Meaning |
|---|---|
| Image | the flash or ROM image: by default the program's own file |
| Board | `5wy19`, `c167cs`, `c167` or `c166` |
| Symbols | optional: an export from `tools/ghidra/ExportOpenc166Symbols.py`, for frame names |
| A/D inputs | `CH=VALUE` pairs; the 5WY19 resets itself unless channel 1 reads above 0C4H |
| Boot mode | 5WY19: erase the application signature so the boot block runs |
| Freeze watchdog | the watchdog never resets the CPU |

The trace starts at the reset vector, stopped. Then:

- **Resume**, **Interrupt**, **Step Into** (one instruction; an EXT or
  ATOMIC sequence counts as one), **Step Over**, **Step Out**.
- **Step Back** and **Resume Back** (in the step menu): the simulator goes
  back by restoring a snapshot and re-running, so the history is exact.
  Resume Back runs to the previous breakpoint or watchpoint hit; with a
  write watchpoint that answers "who last wrote this?".
- Breakpoints from the listing (software and hardware breakpoints are the
  same thing here), and read, write and access watchpoints on any range,
  SFRs included. Toggle and delete them in the Breakpoints window.
- **Kill** resets the simulated CPU (power-on reset); the trace stays.
- Writes to registers and memory go into the simulator (into the flash
  image too) and clear the reverse-execution history.

Each stop is a snapshot in Ghidra's Time window, named after why it
stopped (`breakpoint 1 at 0882D0`, `write 00E79E = 0000 (was 5555)`, ...).

### What the trace holds

- Registers as the C166 module names them: `r0`..`r15`, `IP`, `CSP`, `PSW`,
  `DPP0`..`DPP3`. Other SFRs are memory, as in the module.
- The program counter of the frame as the full `CSP:IP` address (the
  module's `IP` alone is 16 bits).
- Internal RAM and the SFR areas (0F000H..0FFFFH) at every stop; other
  memory when Ghidra reads it.
- The memory map from the simulator's bus: flash, internal RAM, ESFR and
  SFR areas, XRAM and the X-peripherals once enabled, the chip-select
  windows the firmware programs. It is checked again at every stop.
- A module named after the image, so Ghidra maps the trace onto the open
  program of that name, address for address: your labels and comments
  appear in the dynamic listing.

### Without the launcher

In the Debugger, use **Connect via Trace RMI → Accept a single inbound
connection**, then run

```
python3 -m openc166.ghidra firmware.bin --board 5wy19 --adc 1=0x2A0 \
    --ghidra /path/to/ghidra_12.2_PUBLIC --connect 127.0.0.1:PORT
```

or start it with `--listen PORT` and use **Connect to outbound**.

## Limits

- One stack frame: there is no call stack yet.
- The simulator runs in the Python process the launcher starts; the K-line
  bridge (`openc166-sim serve`) is a separate process, so you cannot yet
  break while gkflasher talks to the ECU.
- Tested against a stand-in for Ghidra's Trace RMI server that uses
  Ghidra 12.2's own protocol definitions (python/tests/test_ghidra.py),
  and through the launcher script; the Debugger GUI itself is not covered
  by the tests.

## ELF and DWARF

`openc166 -o prog.elf` writes code assembled with openc166 as ELF with DWARF
line information (see [debug-info.md](debug-info.md)); import it into Ghidra
for source lines next to the listing.
