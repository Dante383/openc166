# SPDX-License-Identifier: MIT
"""The C166/C167 simulator and debugger, scriptable from Python.

    >>> from openc166 import sim
    >>> s = sim.Simulator.from_file("ca663057.bin", board="5wy19")
    >>> s.break_at(0x88046)
    1
    >>> s.cont()
    Stop(reason='breakpoint', id=1)
    >>> s.disassemble(s.pc, 3)
    [Instruction(0x088046, 'MOV     CP, #0F206H'), ...]
    >>> s.r[4], s.sfr["PSW"], s.flag("Z")

Execution is deterministic: the same image and inputs give the same run,
so reverse_step() and reverse_continue() go back by restoring a snapshot
and running forward again. Callbacks that inject input from Python
(attach(), set_gpr() by hand) are not replayed; clear_history() after
changing state by hand.

Locations (breakpoints, watchpoints, goto) may be an int (a physical
address), a symbol name, "file:line" from openc166 debug information, or
for watchpoints an SFR or bit name such as "S0TBUF" or "PSW.Z".
"""

from __future__ import annotations

import csv
import ctypes
import json
import os
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, List, Optional, Tuple, Union

from . import _native

_lib = _native.lib

PathLike = Union[str, "os.PathLike[str]"]
Location = Union[int, str]

__all__ = [
    "Simulator",
    "Stop",
    "Instruction",
    "Unmodelled",
    "Symbols",
    "Snapshot",
    "disassemble",
    "disassemble_module",
    "sfr_address",
    "SimulatorError",
]


class SimulatorError(Exception):
    pass


# ---- Names of registers ------------------------------------------------------


def sfr_address(name: str) -> Optional[int]:
    """Address of a C167 SFR or ESFR by name (any case), or None."""
    a = _lib.oc166_sfr_address(name.encode("ascii", "replace"))
    return None if a < 0 else a


def _bit(name: str) -> Optional[Tuple[int, int]]:
    """(word address, bit) of a bit name (IEN, Z) or "REG.n" (PSW.3, PSW.Z)."""
    if "." in name:
        reg, bit = name.rsplit(".", 1)
        word = sfr_address(reg)
        if word is None:
            return None
        if bit.isdigit():
            return word, int(bit)
        b = _bit(bit)
        if b and b[0] == word:
            return b
        return None
    n = ctypes.c_int()
    a = _lib.oc166_bit_address(name.encode("ascii", "replace"), ctypes.byref(n))
    return None if a < 0 else (a, n.value)


# ---- Disassembly --------------------------------------------------------------


@dataclass(frozen=True)
class Instruction:
    address: int
    size: int
    text: str
    target: Optional[int] = None  # branch target, if static
    data: bytes = b""

    def __repr__(self) -> str:
        return "Instruction(0x{:06x}, {!r})".format(self.address, self.text)


def disassemble(
    data: bytes, address: int = 0, *, count: Optional[int] = None, names: bool = True, cpu: str = "c167"
) -> List[Instruction]:
    """Disassembles bytes that lie at 'address'. Bytes that are not an
    instruction come out as a 2-byte "DW" entry."""
    buf = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(bytes(data) or b"\0")
    text = ctypes.create_string_buffer(128)
    target = ctypes.c_int64()
    flags = (0 if names else 2) | (1 if cpu.lower() == "c166" else 0)
    out: List[Instruction] = []
    off = 0
    ext_left = 0
    while off + 1 < len(data) and (count is None or len(out) < count):
        f = flags | (8 if ext_left > 0 else 0)
        n = _lib.oc166_disassemble(
            ctypes.cast(ctypes.byref(buf, off), ctypes.POINTER(ctypes.c_uint8)),
            len(data) - off,
            (address + off) & 0xFFFFFF,
            f,
            text,
            len(text),
            ctypes.byref(target),
        )
        if n == 0:
            word = data[off] | (data[off + 1] << 8)
            out.append(Instruction(address + off, 2, "DW      0{:04X}H".format(word), None, bytes(data[off : off + 2])))
            off += 2
            ext_left = 0
            continue
        t = text.value.decode("ascii")
        out.append(Instruction(address + off, n, t, target.value if target.value >= 0 else None, bytes(data[off : off + n])))
        mnem = t.split()[0]
        if mnem in ("EXTR", "EXTPR", "EXTSR"):
            ext_left = int(t.rsplit("#", 1)[1])
        elif ext_left:
            ext_left -= 1
        off += n
    return out


def disassemble_module(data: bytes, base: int = 0, *, cpu: str = "c167") -> Tuple[str, int]:
    """The bytes as openc166 source that assembles back to exactly the same
    bytes. Returns (text, number of instructions written as DB because
    their text would not reassemble)."""
    buf = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(bytes(data) or b"\0")
    fallbacks = ctypes.c_size_t()
    p = _lib.oc166_disassemble_module(buf, len(data), base, 1 if cpu.lower() == "c166" else 0, ctypes.byref(fallbacks))
    if not p:
        raise MemoryError("cannot disassemble")
    try:
        return ctypes.string_at(p).decode("ascii"), fallbacks.value
    finally:
        _lib.oc166_free(p)


# ---- Symbols ------------------------------------------------------------------


@dataclass
class SourceLine:
    address: int
    size: int
    file: str
    line: int
    macro: Optional[Tuple[str, int]] = None  # (file, line) of the macro body line
    include: List[Tuple[str, int]] = field(default_factory=list)


class Symbols:
    """Names for addresses: from openc166 debug information (with source
    lines) or from a Ghidra export (functions, labels, comments)."""

    def __init__(self) -> None:
        self.by_name: Dict[str, int] = {}
        self.by_address: Dict[int, str] = {}
        self.functions: Dict[int, str] = {}
        self.comments: Dict[int, str] = {}
        self.lines: List[SourceLine] = []
        self._line_index: Dict[Tuple[str, int], int] = {}

    def add(self, name: str, address: int, function: bool = False) -> None:
        self.by_name[name] = address
        self.by_address.setdefault(address, name)
        if function:
            self.functions[address] = name

    def load_debug_info(self, info: Union[dict, str, PathLike], mirror: int = 0) -> None:
        """Adds openc166 debug information: a dict (Result.debug_info()), a
        JSON file or JSON text. 'mirror' is added to every address, for code
        that runs at a mirror of where it was assembled (e.g. 80000H)."""
        if not isinstance(info, dict):
            text = str(info)
            if not text.lstrip().startswith("{"):
                with open(info, "r", encoding="utf-8") as f:
                    text = f.read()
            info = json.loads(text)
        files = info["files"]
        for s in info.get("symbols", []):
            if s.get("kind") == "label" and s.get("value") is not None and s.get("type") == "number":
                self.add(s["name"], s["value"] + mirror, s.get("label_type") in ("near", "far"))
        for l in info.get("lines", []):
            m = l.get("macro")
            sl = SourceLine(
                l["address"] + mirror,
                l["size"],
                files[l["file"]],
                l["line"],
                (files[m["file"]], m["line"]) if m else None,
                [(files[f], n) for f, n in l.get("include", [])],
            )
            self.lines.append(sl)
            key = (os.path.normcase(os.path.basename(sl.file)), sl.line)
            self._line_index.setdefault(key, sl.address)
            key2 = (os.path.normcase(os.path.abspath(sl.file)), sl.line)
            self._line_index.setdefault(key2, sl.address)
            if m:
                self._line_index.setdefault((os.path.normcase(os.path.basename(files[m["file"]])), m["line"]), sl.address)
        self.lines.sort(key=lambda x: x.address)

    def load_ghidra(self, path: PathLike) -> None:
        """Adds a Ghidra export: the JSON that tools/ghidra/ExportOpenc166Symbols.py
        writes, or a CSV export of Ghidra's Symbol Table window (columns
        "Name" and "Location")."""
        with open(path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        if text.lstrip().startswith("{"):
            data = json.loads(text)
            for fn in data.get("functions", []):
                self.add(fn["name"], _addr(fn["address"]), True)
            for lb in data.get("labels", []):
                self.add(lb["name"], _addr(lb["address"]))
            for c in data.get("comments", []):
                self.comments[_addr(c["address"])] = c["text"]
            return
        for row in csv.DictReader(text.splitlines()):
            name, loc = row.get("Name"), row.get("Location")
            if not name or not loc:
                continue
            try:
                a = _addr(loc)
            except ValueError:
                continue
            self.add(name, a, (row.get("Type") or "").lower() == "function")

    def line_at(self, address: int) -> Optional[SourceLine]:
        """The source line whose bytes include 'address'."""
        import bisect

        i = bisect.bisect_right([l.address for l in self.lines], address) - 1
        if i >= 0 and self.lines[i].address <= address < self.lines[i].address + self.lines[i].size:
            return self.lines[i]
        return None

    def address_of_line(self, file: str, line: int) -> Optional[int]:
        for key in ((os.path.normcase(os.path.abspath(file)), line), (os.path.normcase(os.path.basename(file)), line)):
            if key in self._line_index:
                return self._line_index[key]
        # The next line with code.
        base = os.path.normcase(os.path.basename(file))
        best = None
        for (f, n), a in self._line_index.items():
            if f == base and n >= line and (best is None or n < best[0]):
                best = (n, a)
        return best[1] if best else None

    def name_at(self, address: int) -> Optional[str]:
        return self.by_address.get(address)

    def describe(self, address: int) -> str:
        """"name+offset" for the nearest symbol at or below 'address'."""
        best = None
        for a, n in self.by_address.items():
            if a <= address and address - a < 0x10000 and (best is None or a > best[0]):
                best = (a, n)
        if not best:
            return "0{:X}H".format(address)
        return best[1] if best[0] == address else "{}+0{:X}H".format(best[1], address - best[0])


def _addr(v: Union[int, str]) -> int:
    if isinstance(v, int):
        return v
    s = v.strip()
    if ":" in s:
        # Ghidra writes "ram:00088046", or segment:offset such as "0008:8046".
        prefix, off = s.rsplit(":", 1)
        if len(off) <= 4 and prefix and all(c in "0123456789abcdefABCDEF" for c in prefix):
            return (int(prefix, 16) << 16) | int(off, 16)
        return int(off, 16)
    if s.lower().startswith("0x"):
        return int(s, 16)
    if s.upper().endswith("H"):
        return int(s[:-1], 16)
    return int(s, 16)


# ---- The simulator ---------------------------------------------------------


@dataclass(frozen=True)
class Stop:
    """Why execution stopped.

    reason is "none" (the instruction limit ran out), "breakpoint",
    "watchpoint", "unmodelled", "powerdown", "idle", "halted" or
    "undefined". For a watchpoint, access is ("read" or "write", address,
    size, value, old value).
    """

    reason: str
    id: int = 0
    access: Optional[Tuple[str, int, int, int, int]] = None
    history_start: bool = False

    def __repr__(self) -> str:
        extra = ", access={}".format(self.access) if self.access else ""
        extra += ", history_start=True" if self.history_start else ""
        return "Stop(reason={!r}, id={}{})".format(self.reason, self.id, extra)


@dataclass(frozen=True)
class Unmodelled:
    instruction: int
    pc: int
    address: int
    write: bool
    value: int
    what: str


class Snapshot:
    """The full machine state at one point; Simulator.load() restores it."""

    def __init__(self, handle: int):
        self._handle = handle
        self.instructions: int = _lib.oc166_snapshot_instructions(handle)

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            _lib.oc166_snapshot_free(self._handle)
            self._handle = None


class _Registers:
    def __init__(self, sim: "Simulator"):
        self._sim = sim

    def __getitem__(self, n: int) -> int:
        if not 0 <= n < 16:
            raise IndexError("R0..R15")
        return _lib.oc166_sim_gpr(self._sim._h, n)

    def __setitem__(self, n: int, v: int) -> None:
        if not 0 <= n < 16:
            raise IndexError("R0..R15")
        _lib.oc166_sim_set_gpr(self._sim._h, n, v & 0xFFFF)

    def __iter__(self):
        return iter([self[i] for i in range(16)])

    def __repr__(self) -> str:
        return "[" + ", ".join("R{}={:04X}".format(i, v) for i, v in enumerate(self)) + "]"


class _Sfrs:
    def __init__(self, sim: "Simulator"):
        self._sim = sim

    def _a(self, key: Union[int, str]) -> int:
        if isinstance(key, int):
            return key
        a = sfr_address(key)
        if a is None:
            raise KeyError("unknown SFR {!r}".format(key))
        return a

    def __getitem__(self, key: Union[int, str]) -> int:
        return _lib.oc166_sim_sfr(self._sim._h, self._a(key))

    def __setitem__(self, key: Union[int, str], v: int) -> None:
        _lib.oc166_sim_set_sfr(self._sim._h, self._a(key), v & 0xFFFF)


_WATCH_KINDS = {"read": 1, "write": 2, "rw": 3, "access": 3, "change": 4}


class Simulator:
    """A simulated board with a debugger attached."""

    BOARDS = ("5wy19", "c167cs", "c167", "c166")

    def __init__(self, image: bytes, board: str = "c167", symbols: Optional[Symbols] = None):
        buf = (ctypes.c_uint8 * max(len(image), 1)).from_buffer_copy(bytes(image) or b"\0")
        h = _lib.oc166_sim_new(board.lower().encode("ascii"), buf, len(image))
        if not h:
            raise SimulatorError("cannot create board {!r} (known: {})".format(board, ", ".join(self.BOARDS)))
        self._h = h
        self.board = board
        self.symbols = symbols or Symbols()
        self.r = _Registers(self)
        self.sfr = _Sfrs(self)
        self._callbacks: Dict[int, object] = {}  # keeps ctypes callbacks alive
        self._devices: List[object] = []

    @classmethod
    def from_file(cls, path: PathLike, board: str = "c167", symbols: Optional[Symbols] = None) -> "Simulator":
        with open(path, "rb") as f:
            return cls(f.read(), board, symbols)

    @classmethod
    def from_result(cls, result, board: str = "c167", fill: int = 0xFF) -> "Simulator":
        """A simulator running an assembled program (openc166.Result) as
        ROM from address 0, with its debug information loaded."""
        img = result.image(base=0, fill=fill)
        syms = Symbols()
        syms.load_debug_info(result.debug_info())
        return cls(img.data, board, syms)

    def close(self) -> None:
        if self._h:
            _lib.oc166_sim_free(self._h)
            self._h = None

    def __del__(self) -> None:
        if getattr(self, "_h", None):
            self.close()

    def __enter__(self) -> "Simulator":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ---- run control
    def _stop(self, fn, *args) -> Stop:
        s = _native.Stop()
        if fn(self._h, *args, ctypes.byref(s)) != 0:
            raise SimulatorError("simulator call failed")
        access = None
        if s.access:
            access = (
                "read" if s.access == 1 else "write",
                s.access_address,
                s.access_size,
                s.access_value,
                s.access_old,
            )
        return Stop(s.reason.decode("ascii"), s.id, access, bool(s.history_start))

    def step(self) -> Stop:
        """One instruction; an EXT or ATOMIC sequence counts as one."""
        return self._stop(_lib.oc166_sim_step)

    def cont(self, max_instructions: Optional[int] = None) -> Stop:
        """Runs until a breakpoint, watchpoint or other stop."""
        return self._stop(_lib.oc166_sim_continue, max_instructions or 0)

    run = cont

    def step_over(self, max_instructions: Optional[int] = None) -> Stop:
        return self._stop(_lib.oc166_sim_step_over, max_instructions or 0)

    def step_out(self, max_instructions: Optional[int] = None) -> Stop:
        return self._stop(_lib.oc166_sim_step_out, max_instructions or 0)

    def reverse_step(self) -> Stop:
        return self._stop(_lib.oc166_sim_reverse_step)

    def reverse_continue(self) -> Stop:
        """Back to the last breakpoint or watchpoint hit; with a write
        watchpoint this finds who last wrote an address."""
        return self._stop(_lib.oc166_sim_reverse_continue)

    def run_for(self, seconds: float) -> Stop:
        """Runs for 'seconds' of simulated time (or until a stop)."""
        target = self.cycles + int(seconds * _lib.oc166_sim_clock_hz(self._h))
        return self._stop(_lib.oc166_sim_run_until_cycle, target)

    def run_until_cycle(self, cycle: int) -> Stop:
        return self._stop(_lib.oc166_sim_run_until_cycle, cycle)

    def goto(self, instructions: int) -> None:
        """To the state after 'instructions' instructions."""
        if _lib.oc166_sim_goto(self._h, instructions) != 0:
            raise SimulatorError("instruction {} is before the recorded history".format(instructions))

    def request_stop(self) -> None:
        _lib.oc166_sim_request_stop(self._h)

    def reset(self, kind: str = "power-on") -> None:
        kinds = {"power-on": 0, "software": 1, "watchdog": 2, "hardware": 3}
        _lib.oc166_sim_reset(self._h, kinds[kind])

    def set_history(self, interval: int = 20000, limit: int = 256) -> None:
        _lib.oc166_sim_set_history(self._h, interval, limit)

    def clear_history(self) -> None:
        _lib.oc166_sim_clear_history(self._h)

    @property
    def history_start(self) -> int:
        return _lib.oc166_sim_history_start(self._h)

    # ---- state
    @property
    def pc(self) -> int:
        """CSP:IP as a 24-bit address."""
        return _lib.oc166_sim_pc(self._h)

    @pc.setter
    def pc(self, v: int) -> None:
        _lib.oc166_sim_set_pc(self._h, v & 0xFFFFFF)

    @property
    def ip(self) -> int:
        return self.pc & 0xFFFF

    @property
    def csp(self) -> int:
        return self.pc >> 16

    @property
    def psw(self) -> int:
        return self.sfr["PSW"]

    def flag(self, name: str) -> bool:
        """A PSW flag: N, C, V, Z, E, MULIP, USR0, IEN."""
        bits = {"N": 0, "C": 1, "V": 2, "Z": 3, "E": 4, "MULIP": 5, "USR0": 6, "HLDEN": 10, "IEN": 11}
        return bool((self.psw >> bits[name.upper()]) & 1)

    @property
    def instructions(self) -> int:
        return _lib.oc166_sim_instructions(self._h)

    @property
    def cycles(self) -> int:
        return _lib.oc166_sim_cycles(self._h)

    @property
    def time(self) -> float:
        """Simulated seconds since power-on."""
        return self.cycles / _lib.oc166_sim_clock_hz(self._h)

    @property
    def einit_done(self) -> bool:
        return bool(_lib.oc166_sim_einit_done(self._h))

    @property
    def ext(self) -> dict:
        """The active EXT or ATOMIC sequence, if any."""
        left, esfr, kind, value = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint32()
        _lib.oc166_sim_ext(self._h, ctypes.byref(left), ctypes.byref(esfr), ctypes.byref(kind), ctypes.byref(value))
        return {
            "left": left.value,
            "esfr": bool(esfr.value),
            "kind": ("none", "page", "segment")[kind.value],
            "value": value.value,
        }

    # ---- memory
    def read(self, address: int, size: int) -> bytes:
        """Reads without side effects."""
        buf = (ctypes.c_uint8 * max(size, 1))()
        _lib.oc166_sim_read(self._h, address, buf, size)
        return bytes(buf[:size])

    def write(self, address: int, data: bytes) -> None:
        """Writes without side effects (also into ROM images)."""
        buf = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(bytes(data) or b"\0")
        _lib.oc166_sim_write(self._h, address, buf, len(data))

    def read16(self, address: int) -> int:
        b = self.read(address, 2)
        return b[0] | (b[1] << 8)

    def write16(self, address: int, v: int) -> None:
        self.write(address, bytes((v & 0xFF, (v >> 8) & 0xFF)))

    def data_address(self, a: int) -> int:
        """The physical address of the 16-bit data address 'a' now."""
        return _lib.oc166_sim_data_address(self._h, a & 0xFFFF)

    def describe(self, address: int) -> str:
        """What answers at 'address': "IRAM", "SFR", "CS0:flash", ..."""
        return _lib.oc166_sim_describe(self._h, address).decode("ascii")

    def disassemble(self, address: Optional[int] = None, count: int = 1, names: bool = True) -> List[Instruction]:
        """Instructions from 'address' (default: the PC), as the CPU sees
        them now."""
        a = self.pc if address is None else address
        text = ctypes.create_string_buffer(128)
        target = ctypes.c_int64()
        out = []
        for _ in range(count):
            n = _lib.oc166_sim_disassemble(self._h, a, 0 if names else 2, text, len(text), ctypes.byref(target))
            if n == 0:
                w = self.read16(a)
                out.append(Instruction(a, 2, "DW      0{:04X}H".format(w), None, self.read(a, 2)))
                n = 2
            else:
                out.append(
                    Instruction(a, n, text.value.decode("ascii"), target.value if target.value >= 0 else None, self.read(a, n))
                )
            a = (a & 0xFF0000) | ((a + n) & 0xFFFF)
        return out

    # ---- locations
    def resolve(self, location: Location) -> int:
        """A physical address for an int, a symbol or "file:line"."""
        if isinstance(location, int):
            return location
        loc = location.strip()
        if loc in self.symbols.by_name:
            return self.symbols.by_name[loc]
        if ":" in loc:
            f, _, n = loc.rpartition(":")
            if n.isdigit():
                a = self.symbols.address_of_line(f, int(n))
                if a is not None:
                    return a
        try:
            return _addr(loc)
        except ValueError:
            raise KeyError("unknown location {!r}".format(location)) from None

    # ---- breakpoints and watchpoints
    def break_at(self, location: Location, condition: Optional[Callable[["Simulator"], bool]] = None) -> int:
        """Stops before the instruction at 'location' runs, if 'condition'
        (called with the simulator) is true. Returns the breakpoint id."""
        cb = self._condition(condition)
        bid = _lib.oc166_sim_add_breakpoint(self._h, self.resolve(location), cb, None)
        if bid < 0:
            raise SimulatorError("cannot add breakpoint")
        self._callbacks[bid] = cb
        return bid

    def watch(
        self,
        location: Location,
        kind: str = "write",
        size: int = 2,
        condition: Optional[Callable[["Simulator"], bool]] = None,
    ) -> int:
        """Stops after an access to 'location': kind "read", "write", "rw"
        or "change". 'location' may also be an SFR ("S0TBUF") or a bit
        ("PSW.Z", "IEN"); for a bit, "change" watches just that bit."""
        mask = 0xFFFF
        if isinstance(location, str) and (sfr_address(location) is not None):
            address = sfr_address(location)
        elif isinstance(location, str) and _bit(location) is not None and location not in self.symbols.by_name:
            address, bit = _bit(location)  # type: ignore[misc]
            mask = 1 << bit
        else:
            address = self.resolve(location)
        if kind not in _WATCH_KINDS:
            raise ValueError("kind must be one of " + ", ".join(_WATCH_KINDS))
        cb = self._condition(condition)
        wid = _lib.oc166_sim_add_watchpoint(self._h, address, size, _WATCH_KINDS[kind], mask, cb, None)
        if wid < 0:
            raise SimulatorError("cannot add watchpoint")
        self._callbacks[wid] = cb
        return wid

    def delete(self, id: int) -> None:
        """Removes a breakpoint or watchpoint."""
        if _lib.oc166_sim_remove_breakpoint(self._h, id) != 0 and _lib.oc166_sim_remove_watchpoint(self._h, id) != 0:
            raise KeyError(id)
        self._callbacks.pop(id, None)

    def _condition(self, condition):
        if condition is None:
            return _native.CONDITION()
        sim = self

        def call(_user):
            try:
                return 1 if condition(sim) else 0
            except Exception:  # a broken condition stops, so the user sees it
                import traceback

                traceback.print_exc()
                return 1

        return _native.CONDITION(call)

    # ---- snapshots
    def save(self) -> Snapshot:
        h = _lib.oc166_sim_save(self._h)
        if not h:
            raise MemoryError("cannot save a snapshot")
        return Snapshot(h)

    def load(self, snapshot: Snapshot) -> None:
        if _lib.oc166_sim_load(self._h, snapshot._handle) != 0:
            raise SimulatorError("cannot load the snapshot")

    # ---- inputs
    def interrupt(self, ic: Union[int, str]) -> None:
        """Sets the request flag of an interrupt by its IC register
        ("S0RIC" or its address)."""
        a = ic if isinstance(ic, int) else sfr_address(ic)
        if a is None:
            raise KeyError(ic)
        _lib.oc166_sim_request_interrupt(self._h, a)

    def serial_send(self, data: bytes, framing_error: bool = False) -> None:
        """Bytes from the host arrive on ASC0 (RXD0) now, one frame apart.
        They are logged, so reverse execution replays them."""
        buf = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(bytes(data) or b"\0")
        if _lib.oc166_sim_serial_send(self._h, buf, len(data), 1 if framing_error else 0) != 0:
            raise SimulatorError("this board has no ASC0")

    def serial_break(self, seconds: float) -> None:
        """RXD0 was low for 'seconds' and has just gone high again: a break,
        or the low phase of a K-line fast init (0.025). ASC0 frames it at
        its current baud rate."""
        cycles = int(seconds * _lib.oc166_sim_clock_hz(self._h))
        if _lib.oc166_sim_serial_line_low(self._h, cycles) != 0:
            raise SimulatorError("this board has no ASC0")

    def fast_init(self, payload: bytes) -> None:
        """An ISO 14230 fast init as a tester does it: 25 ms low, 25 ms high,
        then 'payload' (usually StartCommunication)."""
        self.run_for(0.025)
        self.serial_break(0.025)
        self.run_for(0.025)
        self.serial_send(payload)

    def serial_receive(self, baud: Optional[int] = None) -> bytes:
        """What the CPU sent on ASC0 since the last call. With 'baud', only
        the bytes sent within 3 % of that rate: what a tester listening at
        'baud' receives."""
        return bytes(b for b, rate in self.serial_receive_baud() if baud is None or abs(rate - baud) <= baud * 0.03)

    def serial_receive_baud(self) -> List[Tuple[int, int]]:
        """What the CPU sent on ASC0 since the last call, as (byte, baud)."""
        out: List[Tuple[int, int]] = []
        buf = (ctypes.c_uint8 * 256)()
        rates = (ctypes.c_uint32 * 256)()
        while True:
            n = _lib.oc166_sim_serial_receive_baud(self._h, buf, rates, 256)
            out += [(buf[i], rates[i]) for i in range(n)]
            if n < 256:
                return out

    @property
    def serial_frame_time(self) -> float:
        """Seconds one ASC0 frame takes at its current settings."""
        return _lib.oc166_sim_serial_frame_cycles(self._h) / _lib.oc166_sim_clock_hz(self._h)

    @property
    def serial_echo(self) -> bool:
        return getattr(self, "_echo", False)

    @serial_echo.setter
    def serial_echo(self, v: bool) -> None:
        """ASC0 hears what it sends, as on a K-line."""
        self._echo = v
        self._option("serial_echo", v)

    def set_adc(self, channel: int, value: int) -> None:
        """The 10-bit value A/D channel 'channel' converts to."""
        if _lib.oc166_sim_set_adc(self._h, channel, value & 0x3FF) != 0:
            raise SimulatorError("this board has no ADC channel {}".format(channel))

    def nmi(self) -> None:
        _lib.oc166_sim_nmi(self._h)

    def attach(
        self,
        chip_select: int,
        read: Optional[Callable[[int], int]] = None,
        write: Optional[Callable[[int, int], None]] = None,
        name: str = "python",
    ) -> None:
        """Puts a device written in Python on chip select 0..4: read(address)
        returns a byte, write(address, value) receives one. Its state is not
        part of snapshots."""
        r = _native.READ_FN(lambda _u, a: (read(a) & 0xFF) if read else 0xFF)
        w = _native.WRITE_FN(lambda _u, a, v: write(a, v) if write else None)
        if _lib.oc166_sim_set_chip_select(self._h, chip_select, name.encode(), r, w, None) != 0:
            raise SimulatorError("cannot attach a device to CS{}".format(chip_select))
        self._devices.append((r, w))

    # ---- options and diagnostics
    def _option(self, name: str, value: bool) -> None:
        _lib.oc166_sim_set_option(self._h, name.encode(), 1 if value else 0)

    @property
    def stop_on_unmodelled(self) -> bool:
        return getattr(self, "_sou", False)

    @stop_on_unmodelled.setter
    def stop_on_unmodelled(self, v: bool) -> None:
        self._sou = v
        self._option("stop_on_unmodelled", v)

    @property
    def watchdog_frozen(self) -> bool:
        return getattr(self, "_wf", False)

    @watchdog_frozen.setter
    def watchdog_frozen(self, v: bool) -> None:
        """While frozen the watchdog never resets the CPU."""
        self._wf = v
        self._option("watchdog_frozen", v)

    @property
    def unmodelled(self) -> List[Unmodelled]:
        """Accesses to registers and addresses nothing models, each once."""
        out = []
        u = _native.Unmodelled()
        for i in range(_lib.oc166_sim_unmodelled_count(self._h)):
            _lib.oc166_sim_unmodelled(self._h, i, ctypes.byref(u))
            out.append(Unmodelled(u.instruction, u.pc, u.address, bool(u.write), u.value, u.what.decode("ascii", "replace")))
        return out

    def clear_unmodelled(self) -> None:
        _lib.oc166_sim_clear_unmodelled(self._h)

    def where(self) -> str:
        """The PC as symbol+offset and source line, if known."""
        pc = self.pc
        s = self.symbols.describe(pc)
        line = self.symbols.line_at(pc)
        if line:
            s += " ({}:{})".format(line.file, line.line)
        return s

    def __repr__(self) -> str:
        return "<openc166.sim.Simulator board={} pc={:06X} instructions={}>".format(self.board, self.pc, self.instructions)


# ---- Command line -------------------------------------------------------------


def _int(text: str) -> int:
    return _addr(text) if not text.isdigit() else int(text)


def main(argv: Optional[List[str]] = None) -> int:
    """openc166-sim: disassemble images and run firmware headless."""
    import argparse

    p = argparse.ArgumentParser(prog="openc166-sim", description=main.__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("disasm", help="disassemble a binary image")
    d.add_argument("image")
    d.add_argument("--base", type=_int, default=0, help="address of the first byte (default 0)")
    d.add_argument("--start", type=_int, help="first address to show (default: base)")
    d.add_argument("--count", type=int, default=64, help="instructions to show")
    d.add_argument("--source", action="store_true",
                   help="write the whole image as source that reassembles to the same bytes")
    d.add_argument("--symbols", help="Ghidra export (JSON or CSV) for names")

    r = sub.add_parser("run", help="run firmware and report what it touched that is not modelled")
    r.add_argument("image")
    r.add_argument("--board", default="c167", choices=Simulator.BOARDS)
    r.add_argument("--instructions", type=_int, default=1_000_000)
    r.add_argument("--break", dest="breaks", action="append", default=[], help="stop at an address or symbol")
    r.add_argument("--symbols", help="Ghidra export (JSON or CSV)")
    r.add_argument("--freeze-watchdog", action="store_true")
    r.add_argument("--trace", type=int, default=0, help="print the last N instructions")

    a = p.parse_args(argv)
    syms = Symbols()
    if getattr(a, "symbols", None):
        syms.load_ghidra(a.symbols)
    with open(a.image, "rb") as f:
        data = f.read()

    if a.cmd == "disasm":
        if a.source:
            text, fallbacks = disassemble_module(data, a.base)
            sys_stdout_write(text)
            if fallbacks:
                print("; {} instruction(s) kept as DB: no A166 spelling gives their bytes".format(fallbacks),
                      file=__import__("sys").stderr)
            return 0
        start = a.base if a.start is None else a.start
        off = start - a.base
        for ins in disassemble(data[off:], start, count=a.count):
            name = syms.name_at(ins.address)
            if name:
                print("{}:".format(name))
            print("{:06X}  {:<12} {}".format(ins.address, ins.data.hex().upper(), ins.text))
        return 0

    s = Simulator(data, a.board, syms)
    s.watchdog_frozen = a.freeze_watchdog
    for b in a.breaks:
        s.break_at(b)
    trace: List[Tuple[int, str]] = []
    if a.trace:
        import collections

        trace = collections.deque(maxlen=a.trace)  # type: ignore[assignment]
        while s.instructions < a.instructions:
            ins = s.disassemble(None, 1)[0]
            trace.append((ins.address, ins.text))
            stop = s.step()
            if stop.reason != "none":
                break
        else:
            stop = Stop("none")
    else:
        stop = s.cont(a.instructions)
    for addr, text in trace:
        print("{:06X}  {}".format(addr, text))
    print("stopped: {} at {} after {} instructions ({:.3f} ms)".format(
        stop.reason, s.where(), s.instructions, s.time * 1e3))
    um = s.unmodelled
    print("{} unmodelled access(es):".format(len(um)))
    groups: Dict[str, List[Unmodelled]] = {}
    for u in um:
        key = u.what if u.what.startswith(("SFR", "ESFR")) else u.what.split(" ")[0]
        groups.setdefault(key, []).append(u)
    for key, us in sorted(groups.items(), key=lambda kv: kv[1][0].instruction):
        u = us[0]
        print("  {:<30} first {} at {:06X} (instruction {}){}".format(
            key, "write" if u.write else "read", u.pc, u.instruction,
            "" if len(us) == 1 else ", {} addresses".format(len(us))))
    return 0


def sys_stdout_write(text: str) -> None:
    import sys

    sys.stdout.write(text)


if __name__ == "__main__":
    raise SystemExit(main())
