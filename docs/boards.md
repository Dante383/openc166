# Board files

A board file tells the simulator what hardware the firmware runs on: which
chip, which devices sit on the chip selects, how the chip is configured at
reset, and what the inputs read before a script changes them. Board files are
JSON. Comments (`//` and `/* */`) are allowed; trailing commas and duplicate
members are errors.

Everywhere a board is taken (`--board` in `openc166-sim run`/`serve`,
`python -m openc166.ghidra` and the Ghidra launcher, `board=` in
`Simulator(...)`, `"board"` in a DAP launch configuration), you can give:

- the name of a built-in board: `c166`, `c167`, `c167cr`, `c167cs`, `5wy19`
  (their files are in [boards/](../boards));
- the path of a `.json` file (anything containing a `/`, or `./my.json`);
- the JSON text itself (it starts with `{`).

Mistakes are reported with the place:

```
my-ecu.json: line 14, column 20: unknown member "colck_hz" (expected format, version, name, ...)
```

The firmware image you load is the board's `$IMAGE`. Things that depend on
the firmware rather than the board, such as `--boot-mode` or a script that
plays a crank signal, stay outside the board file.

## An example

The 5WY19 file, shortened:

```jsonc
{
  "format": "openc166-board",
  "version": 1,
  "name": "Siemens 5WY19",
  "chip": "c167cs",
  "clock_hz": "24MHz",
  "chip_selects": {
    // Only A0..A18 reach the flash, so it appears at 0 and at 80000H.
    "cs0": { "device": "flash", "model": "am29f400bb", "image": "$IMAGE", "mask": "0x7FFFF" },
    "cs2": { "device": "ram", "size": "64K" },
    "cs3": { "device": "latch" }
  },
  "inputs": { "adc": { "1": "0x2A0" } },
  "options": { "serial_echo": true }
}
```

## Numbers

A number is a JSON number or a string: `"0x7FFFF"`, `"7FFFFH"`, sizes with
`K` or `M` (`"64K"` is 65536, `"1M"` 1048576), frequencies with `Hz`, `kHz`
or `MHz` (`"24MHz"`). Underscores are ignored (`"0x7_FFFF"`).

## Top level

| Member | |
|---|---|
| `format` | `"openc166-board"` (optional) |
| `version` | `1` (optional) |
| `name` | shown by the debugger and in errors |
| `description` | free text |
| `chip` | **required**: `c166`, `c167`, `c167cr` or `c167cs` |
| `clock_hz` | fCPU; default 25 MHz. Every peripheral times itself from it |
| `reset` | register values the chip latches at reset |
| `chip_selects` | the external devices, `cs0` .. `cs4` |
| `inputs` | default input values |
| `ssc` | the SSC's default reply |
| `options` | simulator options the board needs |

Any other member is an error, so a misspelt name does not go unnoticed.

## Chips

| Chip | Core | IRAM | On the chip besides the C167 peripherals |
|---|---|---|---|
| `c166` | C166 | 0FA00H, 1 KB | — |
| `c167` | C167 | 0F600H, 2 KB | — |
| `c167cr` | C167 | 0F600H, 2 KB | XRAM 0E000H (2 KB) and CAN at 0EF00H, on with SYSCON.XPEN |
| `c167cs` | C167 | 0F200H, 3 KB | XRAM1 0E000H (2 KB), XRAM2 0C000H (6 KB), CAN1 0EF00H, CAN2 0EE00H, each on with its XPERCON bit; XPERCON resets to 0401H |

Every chip has the C167 interrupt table and the modelled peripherals
(ASC0, CAPCOM, SSC, A/D converter, watchdog; see
[simulator.md](simulator.md)). The CAN modules are logged storage.

## Reset configuration

`reset` sets what the chip reads from the bus at reset (the data bus
configuration pins) and the XPERCON reset value:

| Member | Default |
|---|---|
| `syscon` | 0000H |
| `buscon0` | 0000H |
| `rp0h` | 0FFH |
| `xpercon` | the chip's |

## Chip selects

`chip_selects` maps `cs0` .. `cs4` to devices; `null` or a missing entry
leaves the chip select empty (accesses to it are reported as unmodelled).
CS0 is active from reset; the others answer where the firmware's
ADDRSELx/BUSCONx put them. Each device has a `device` type and an optional
`name`, which the debugger shows (`CS0:flash`).

The address a device sees is masked with `mask`, which is the size rounded
up to a power of two less one unless given. A smaller mask than the address
range makes the device appear several times (mirrors).

### rom

Read-only memory. Writes are reported as unmodelled.

| Member | |
|---|---|
| `image` | `"$IMAGE"` or a file name, relative to the board file |
| `offset` | bytes to skip at the start of the image |
| `size` | default: the image's size |
| `fill` | the byte after the end of the image; default 0FFH |
| `mask` | see above |

`image` or `size` is required.

### ram

Read-write memory: the same members as `rom`; `size` is required unless
there is an `image` to start from. Without an image the RAM holds 0FFH (or
`fill`).

### flash

An AMD-style flash chip with its command set:

- the unlock cycles (AAH at 555H, 55H at 2AAH as word addresses on a 16-bit
  bus, 0AAAH/555H as byte addresses on an 8-bit one), reset (0F0H);
- autoselect (90H): manufacturer, device ID, sector protection (never);
- program (A0H): bits only go from 1 to 0;
- sector erase (80H, 30H) with the 50 µs window for more sectors, and chip
  erase (80H, 10H);
- status polling while the chip works: DQ7 (the complement of the data
  bit while programming, 0 while erasing), DQ6 toggling, DQ3 (the erase has
  started), DQ2 toggling in the sectors being erased.

The operations take virtual time, and their state and the array are part of
snapshots, so reverse execution steps back over a flash write. A write
outside a command is reported as unmodelled. Not modelled: erase suspend
(an erase is finished at once instead), sector protection, DQ5 (time
limit exceeded).

| Member | |
|---|---|
| `model` | **required**: `am29f400bb` or `am29f400bt` (512 KB, bottom or top boot block) |
| `image`, `offset`, `fill`, `mask` | as for `rom`; the size is the chip's |
| `bus_width` | 16 (default) or 8 |
| `program_us` | time to program a word; default 12 |
| `sector_erase_ms` | per sector; default 1000 |
| `chip_erase_ms` | default 8000 |

### latch

An output register (such as a 74HC574): it keeps the last byte written,
which the debugger shows; reads return 0FFH, as from a write-only latch. The 5WY19's latch is the shadow byte the startup code writes
at 0A000H.

### stub

Logged storage of `size` bytes (default 4 KB): accesses work as RAM and are
reported as unmodelled, so a run
shows what the firmware expects there.

## Inputs and options

```jsonc
"inputs":  { "adc": { "1": "0x2A0", "4": 512 } },   // A/D channel: 10-bit value
"ssc":     { "default_reply": "0xFFFF" },           // what an SSC transfer reads back
"options": {
  "serial_echo": true,       // ASC0 hears its own output, as on a K-line
  "watchdog_frozen": false   // the watchdog never times out
}
```

Scripts and command-line options (`--adc`, `--freeze-watchdog`) still
change these after the board is built.

## Writing a board for another ECU

Start from the chip's file in [boards/](../boards), copy it next to the
image, and add what the schematic or the firmware shows. `openc166-sim run
image.bin --board ./my-ecu.json` lists the SFRs and addresses the firmware
touches that nothing models, with the PC of the first access, which is
usually the quickest way to find the next device to describe.
