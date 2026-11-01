# MTOS payloads

Small programs for Siemens SIMK43 ECUs (Infineon C167), run through MTOS, a
diagnostic feature of the stock firmware. They are the payloads of
[gkflasher](https://github.com/dante383/gkflasher), written as Keil A166
source. Each one gives exactly the bytes that gkflasher's own generator
(`flasher/mtos_asm.py`) produces, and Keil A166 gives the same bytes too.
The `mtos_*` tests in `tests/CMakeLists.txt` check this.

How MTOS runs a payload:

- The tester uploads a blob to RAM at `0C000H` and starts it with the
  KWP2000 service StartRoutineByLocalIdentifier `0FAH`.
- The blob starts with a 20-byte header: a CRC of the code, the range the
  CRC covers, a 6-byte signature that must match the ECU's flash at
  `3FE8H`, and the letters `MT`. The tool that uploads the payload builds
  this header (gkflasher's `build_blob()`), so these files hold only the
  code, which starts at `0C014H`.
- The firmware calls `0C014H` with a far call, so a payload is a `FAR`
  procedure and ends with `RETS`. Keil turns `RET` into `RETS` inside a
  `FAR` procedure.
- While the payload runs, the firmware waits. The payload has the K-line
  (the ASC0 UART) to itself, and must service the watchdog (`SRVWDT`) in
  long loops, or the ECU resets.

| File | What it does |
|---|---|
| `marker.a66` | writes `0BEEFH` to `0C002H`: proof that the code ran |
| `kline_hello.a66` | sends `MTOS` over the K-line |
| `memory_dump.a66` | sends a block of memory over the K-line |
| `loader.a66` | receives a larger program ("stage 2") over the K-line into `0E000H` and runs it |
| `stage2_hello.a66` | a stage 2 that sends `AA 55 C2 "OK"` |
| `bsl_shim.a66` | a stage 2 that prepares the ECU for the stock BSL kernel, which is appended to it |
| `driver_patch.a66` | a patch that makes the stock flash driver service the watchdog while erasing |

Assemble one with:

```
openc166 marker.a66
```

This writes `marker.bin`, the raw code from `0C014H` on, and
`openc166 marker.a66 -l marker.lst` also writes a listing.
