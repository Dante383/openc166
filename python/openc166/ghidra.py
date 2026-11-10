# SPDX-License-Identifier: MIT
"""Ghidra Debugger back-end for the openc166 simulator (Trace RMI).

Ghidra's Debugger (10.3 and later) drives debuggers through Trace RMI: the
back-end connects to Ghidra, records the target's state into a trace, and
offers methods (resume, step, breakpoints, ...) that the Debugger's
buttons call. This module is such a back-end for openc166.sim. It records
a snapshot at every stop: the registers, the internal RAM and SFRs, and the
program counter as a full CSP:IP address, so the dynamic listing follows
execution in your annotated program.

Use it from Ghidra with the "openc166" launcher (tools/ghidra/
openc166-debugger, see docs/ghidra.md), or by hand: in Ghidra's Debugger
choose "Connect via Trace RMI" -> "Accept a single inbound connection",
then run

    python -m openc166.ghidra --connect 127.0.0.1:PORT firmware.bin --board 5wy19

ghidratrace, Ghidra's Python client, comes with Ghidra: it is found
through MODULE_Debugger_rmi_trace_HOME (set by Ghidra's launchers),
GHIDRA_INSTALL_DIR or --ghidra, or as an installed package.

Register names follow the C166 processor module (r0..r15, IP, CSP, PSW,
DPP0..DPP3); other SFRs are memory (the ram space).
"""

import argparse
import concurrent.futures
import os
import socket
import sys
import threading
from typing import Annotated, Any, Dict, List, Optional, Tuple

from . import sim as simmod

LANGUAGE = "C166:LE:16:default"
COMPILER = "tasking"

PROCESS = "Processes[0]"
THREAD = PROCESS + ".Threads[0]"
FRAME = THREAD + ".Stack[0]"
REGS = FRAME + ".Registers"
MEMORY = PROCESS + ".Memory"
MODULES = PROCESS + ".Modules"
PROC_BREAKS = PROCESS + ".Breakpoints"
BREAKS = "Breakpoints"

SCHEMA = """<?xml version="1.0"?>
<context>
  <schema name="Openc166Session" elementResync="NEVER" attributeResync="NEVER">
    <interface name="EventScope"/>
    <interface name="FocusScope"/>
    <interface name="Aggregate"/>
    <element schema="VOID"/>
    <attribute name="Processes" schema="ProcessContainer" required="yes" fixed="yes"/>
    <attribute name="Breakpoints" schema="BreakpointContainer" required="yes" fixed="yes"/>
    <attribute name="_event_thread" schema="OBJECT" hidden="yes"/>
    <attribute name="_focus" schema="Selectable" hidden="yes"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="Selectable" elementResync="NEVER" attributeResync="NEVER">
    <element schema="OBJECT"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="ProcessContainer" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <element schema="Process"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="Process" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Activatable"/>
    <interface name="Process"/>
    <interface name="Aggregate"/>
    <interface name="ExecutionStateful"/>
    <element schema="VOID"/>
    <attribute name="Threads" schema="ThreadContainer" required="yes" fixed="yes"/>
    <attribute name="Breakpoints" schema="BreakpointLocationContainer" required="yes" fixed="yes"/>
    <attribute name="Environment" schema="Environment" required="yes" fixed="yes"/>
    <attribute name="Memory" schema="Memory" required="yes" fixed="yes"/>
    <attribute name="Modules" schema="ModuleContainer" required="yes" fixed="yes"/>
    <attribute name="State" schema="EXECUTION_STATE" required="yes" hidden="yes"/>
    <attribute-alias from="_state" to="State"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute name="_short_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="Environment" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Environment"/>
    <element schema="VOID"/>
    <attribute name="OS" schema="STRING"/>
    <attribute name="Arch" schema="STRING"/>
    <attribute name="Endian" schema="STRING"/>
    <attribute name="Debugger" schema="STRING"/>
    <attribute-alias from="_os" to="OS"/>
    <attribute-alias from="_arch" to="Arch"/>
    <attribute-alias from="_endian" to="Endian"/>
    <attribute-alias from="_debugger" to="Debugger"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="ThreadContainer" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <element schema="Thread"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="Thread" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Activatable"/>
    <interface name="Thread"/>
    <interface name="ExecutionStateful"/>
    <interface name="Aggregate"/>
    <element schema="VOID"/>
    <attribute name="Stack" schema="Stack" required="yes" fixed="yes"/>
    <attribute name="TID" schema="LONG"/>
    <attribute-alias from="_tid" to="TID"/>
    <attribute name="State" schema="EXECUTION_STATE" required="yes" hidden="yes"/>
    <attribute-alias from="_state" to="State"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute name="_short_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="Stack" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Stack"/>
    <element schema="StackFrame"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="StackFrame" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Activatable"/>
    <interface name="StackFrame"/>
    <interface name="Aggregate"/>
    <element schema="VOID"/>
    <attribute name="Registers" schema="RegisterValueContainer" required="yes" fixed="yes"/>
    <attribute name="PC" schema="ADDRESS" required="yes"/>
    <attribute-alias from="_pc" to="PC"/>
    <attribute name="Function" schema="STRING" hidden="yes"/>
    <attribute-alias from="_function" to="Function"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="RegisterValueContainer" attributeResync="ONCE">
    <interface name="RegisterContainer"/>
    <attribute schema="Register"/>
  </schema>
  <schema name="Register">
    <interface name="Register"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="Memory" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Memory"/>
    <element schema="MemoryRegion"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="MemoryRegion" elementResync="NEVER" attributeResync="NEVER">
    <interface name="MemoryRegion"/>
    <element schema="VOID"/>
    <attribute name="Range" schema="RANGE" required="yes"/>
    <attribute-alias from="_range" to="Range"/>
    <attribute name="_readable" schema="BOOL" required="yes" hidden="yes"/>
    <attribute name="_writable" schema="BOOL" required="yes" hidden="yes"/>
    <attribute name="_executable" schema="BOOL" required="yes" hidden="yes"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="ModuleContainer" canonical="yes" elementResync="ONCE" attributeResync="NEVER">
    <element schema="Module"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="Module" elementResync="NEVER" attributeResync="NEVER">
    <interface name="Module"/>
    <element schema="VOID"/>
    <attribute name="Name" schema="STRING"/>
    <attribute-alias from="_module_name" to="Name"/>
    <attribute name="Range" schema="RANGE"/>
    <attribute-alias from="_range" to="Range"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="BreakpointContainer" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <element schema="BreakpointSpec"/>
    <attribute schema="VOID"/>
  </schema>
  <schema name="BreakpointSpec" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <interface name="BreakpointSpec"/>
    <interface name="Togglable"/>
    <element schema="BreakpointLocation"/>
    <attribute name="Expression" schema="STRING" hidden="yes"/>
    <attribute-alias from="_expression" to="Expression"/>
    <attribute name="Kinds" schema="STRING" required="yes" hidden="yes"/>
    <attribute-alias from="_kinds" to="Kinds"/>
    <attribute name="Enabled" schema="BOOL" required="yes"/>
    <attribute-alias from="_enabled" to="Enabled"/>
    <attribute name="Hit Count" schema="INT"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="BreakpointLocation" elementResync="NEVER" attributeResync="NEVER">
    <interface name="BreakpointLocation"/>
    <element schema="VOID"/>
    <attribute name="Range" schema="RANGE"/>
    <attribute-alias from="_range" to="Range"/>
    <attribute name="Enabled" schema="BOOL"/>
    <attribute-alias from="_enabled" to="Enabled"/>
    <attribute name="_display" schema="STRING" hidden="yes"/>
    <attribute schema="ANY"/>
  </schema>
  <schema name="BreakpointLocationContainer" canonical="yes" elementResync="NEVER" attributeResync="NEVER">
    <element schema="BreakpointLocation"/>
    <attribute schema="VOID"/>
  </schema>
</context>
"""


def import_ghidratrace(ghidra: Optional[str] = None):
    """Finds Ghidra's Python client: MODULE_Debugger_rmi_trace_HOME (set by
    Ghidra's launchers), the --ghidra directory, GHIDRA_INSTALL_DIR, or an
    installed package."""
    candidates = []
    home = os.getenv("MODULE_Debugger_rmi_trace_HOME")
    if home:
        candidates += [os.path.join(home, "pypkg", "src"), os.path.join(home, "build", "pypkg", "src")]
    for root in (ghidra, os.getenv("GHIDRA_INSTALL_DIR")):
        if root:
            candidates.append(os.path.join(root, "Ghidra", "Debug", "Debugger-rmi-trace", "pypkg", "src"))
    for c in candidates:
        if os.path.isdir(os.path.join(c, "ghidratrace")) and c not in sys.path:
            sys.path.insert(0, c)
            break
    try:
        import ghidratrace.client  # noqa: F401
    except Exception as e:  # ImportError, or a protobuf older than Ghidra's generated code
        raise SystemExit(
            "cannot import ghidratrace (Ghidra's Trace RMI client): run from Ghidra's launcher, "
            "or pass --ghidra /path/to/ghidra_x.y_PUBLIC ({})".format(e)
        )
    return ghidratrace


# The back-end is built once ghidratrace is importable: its method
# annotations need Ghidra's types.
def make_backend(gt_client):
    Client = gt_client.Client
    MethodRegistry = gt_client.MethodRegistry
    Address = gt_client.Address
    AddressRange = gt_client.AddressRange
    RegVal = gt_client.RegVal
    TraceObject = gt_client.TraceObject
    ParamDesc = gt_client.ParamDesc

    # Parameter types: their names are schema names.
    class Process(TraceObject):
        pass

    class Thread(TraceObject):
        pass

    class StackFrame(TraceObject):
        pass

    class RegisterValueContainer(TraceObject):
        pass

    class Memory(TraceObject):
        pass

    class BreakpointSpec(TraceObject):
        pass

    class BreakpointContainer(TraceObject):
        pass

    executor = concurrent.futures.ThreadPoolExecutor(max_workers=1, thread_name_prefix="openc166-ghidra")
    registry = MethodRegistry(executor)

    class Backend:
        def __init__(self, s: simmod.Simulator, program: str, memory_limit: int = 0x100000):
            self.s = s
            self.program = program
            self.memory_limit = memory_limit
            self.client = None
            self.trace = None
            self.lock = threading.RLock()
            self.running = False
            self.done = threading.Event()
            # Ghidra's breakpoint number -> spec: kind ('x', 'R', 'W', 'RW'),
            # address, size, enabled, the simulator's id.
            self.breaks: Dict[int, Dict[str, Any]] = {}
            self.regions: List[Tuple[int, int, str]] = []
            self.next_break = 1

        # ---- connection and trace
        def connect(self, address: str) -> None:
            host, port = address.rsplit(":", 1)
            sock = socket.socket()
            sock.connect((host, int(port)))
            # Messages are a length, then the body: without this, Nagle's
            # algorithm can hold a body back while the length is unacknowledged.
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.client = Client(sock, "openc166 simulator", registry)

        def listen(self, address: str) -> None:
            host, port = address.rsplit(":", 1) if ":" in address else ("127.0.0.1", address)
            server = socket.socket()
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((host, int(port)))
            server.listen(1)
            print("listening for Ghidra at {}:{}".format(*server.getsockname()), flush=True)
            sock, _ = server.accept()
            server.close()
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.client = Client(sock, "openc166 simulator", registry)

        def start(self, name: str) -> None:
            self.trace = self.client.create_trace(name, LANGUAGE, COMPILER, extra=None)
            with self.trace.open_tx("Create root"):
                root = self.trace.create_root_object(SCHEMA, "Openc166Session")
                root.set_value("_display", "openc166 simulator ({})".format(self.s.board))
            with self.trace.open_tx("Create objects"):
                self.trace.snapshot("Power on")
                for path in (PROCESS, THREAD, FRAME, REGS, MEMORY, MODULES, PROC_BREAKS, BREAKS,
                             PROCESS + ".Threads", THREAD + ".Stack", PROCESS + ".Environment",
                             "Processes"):
                    self.trace.create_object(path).insert()
                proc = self.trace.proxy_object_path(PROCESS)
                proc.set_value("_display", "C167 ({})".format(self.s.board))
                env = self.trace.proxy_object_path(PROCESS + ".Environment")
                env.set_value("_os", "none")
                env.set_value("_arch", "C166")
                env.set_value("_endian", "little")
                env.set_value("_debugger", "openc166")
                self.trace.proxy_object_path(THREAD).set_value("_display", "CPU")
                self.trace.proxy_object_path(THREAD).set_value("_tid", 0)
                self.put_regions()
                self.put_module()
                self.put_state(running=False)
                self.put_frame()
            self.activate()

        # ---- recording
        def put_regions(self) -> None:
            """Memory regions from the simulator's bus routing: what answers
            where in the first 'memory_limit' bytes. The firmware changes the
            map (chip-select windows, XRAM), so it is checked at every stop."""
            regions: List[Tuple[int, int, str]] = []
            a = 0
            while a < self.memory_limit:
                step = 0x100 if 0xC000 <= a < 0x10000 else 0x1000
                what = self.s.describe(a)
                if regions and regions[-1][2] == what and regions[-1][1] == a:
                    regions[-1] = (regions[-1][0], a + step, what)
                else:
                    regions.append((a, a + step, what))
                a += step
            regions = [r for r in regions if not (r[2].endswith(":none") or r[2] == "reserved")]
            if regions == self.regions:
                return
            self.regions = regions
            self.trace.proxy_object_path(MEMORY).retain_values(["[{:06x}]".format(r[0]) for r in regions])
            for start, end, what in regions:
                key = "{}[{:06x}]".format(MEMORY, start)
                obj = self.trace.create_object(key)
                obj.set_value("_range", AddressRange("ram", start, end - 1))
                obj.set_value("_display", "{} {:06X}..{:06X}".format(what, start, end - 1))
                code = what.startswith("CS")
                obj.set_value("_readable", True)
                obj.set_value("_writable", not what.endswith(("flash", "rom")))
                obj.set_value("_executable", code or what == "IRAM")
                obj.insert()

        def put_module(self) -> None:
            """A module named after the program, so Ghidra maps the trace onto
            the open program of that name (the same addresses)."""
            obj = self.trace.create_object('{}["{}"]'.format(MODULES, self.program))
            obj.set_value("_module_name", self.program)
            obj.set_value("_range", AddressRange("ram", 0, self.memory_limit - 1))
            obj.insert()

        def put_state(self, running: bool) -> None:
            state = "RUNNING" if running else "STOPPED"
            for path in (PROCESS, THREAD):
                self.trace.proxy_object_path(path).set_value("_state", state)

        def put_frame(self) -> None:
            s = self.s
            values = [RegVal("r{}".format(i), s.r[i].to_bytes(2, "big")) for i in range(16)]
            values.append(RegVal("IP", s.ip.to_bytes(2, "big")))
            for name in ("CSP", "PSW", "DPP0", "DPP1", "DPP2", "DPP3"):
                values.append(RegVal(name, s.sfr[name].to_bytes(2, "big")))
            self.trace.put_registers(REGS, values)
            regs = self.trace.proxy_object_path(REGS)
            for v in values:
                regs.set_value(v.name, "0x{:04X}".format(int.from_bytes(v.value, "big")))
            # Internal RAM and SFRs change all the time: record them at every
            # stop. Other memory is read when Ghidra asks for it.
            self.put_memory(0xF000, 0x1000)
            frame = self.trace.proxy_object_path(FRAME)
            frame.set_value("_pc", Address("ram", s.pc))
            frame.set_value("_function", s.symbols.describe(s.pc))
            frame.set_value("_display", "#0 {:06X} {}".format(s.pc, s.where()))

        def put_memory(self, start: int, length: int) -> None:
            data = self.s.read(start, length)
            self.trace.put_bytes(Address("ram", start), data)

        def put_breakpoint(self, number: int) -> None:
            b = self.breaks[number]
            spec_path = "{}[{}]".format(BREAKS, number)
            spec = self.trace.create_object(spec_path)
            spec.set_value("_expression", "0x{:06X}".format(b["address"]))
            spec.set_value("_kinds", {"x": "SW_EXECUTE", "R": "READ", "W": "WRITE", "RW": "READ,WRITE"}[b["kind"]])
            spec.set_value("_enabled", b["enabled"])
            spec.set_value("_display", "{} {:06X}".format(b["kind"], b["address"]))
            loc = self.trace.create_object(spec_path + "[1]")
            loc.set_value("_range", AddressRange("ram", b["address"], b["address"] + b["size"] - 1))
            loc.set_value("_enabled", b["enabled"])
            loc.insert()
            self.trace.proxy_object_path(PROC_BREAKS).set_value("[{}.1]".format(number), loc)
            spec.insert()

        def remove_breakpoint(self, number: int) -> None:
            self.trace.proxy_object_path("{}[{}]".format(BREAKS, number)).remove(tree=True)
            keys = ["[{}.1]".format(n) for n in self.breaks]
            self.trace.proxy_object_path(PROC_BREAKS).retain_values(keys)

        def record_stop(self, description: str) -> None:
            with self.lock, self.trace.open_tx("Stop"):
                self.trace.snapshot("{} at {:06X} ({})".format(description, self.s.pc, self.s.instructions))
                self.put_state(running=False)
                self.put_regions()
                self.put_frame()
            self.activate()

        def activate(self) -> None:
            try:
                self.trace.proxy_object_path(FRAME).activate()
            except Exception:
                pass

        # ---- execution
        def require_stopped(self) -> None:
            if self.running:
                raise RuntimeError("the simulator is running")

        def run_async(self, what, description) -> None:
            self.require_stopped()
            self.running = True
            with self.trace.open_tx("Resume"):
                self.put_state(running=True)

            def worker():
                try:
                    stop = what()
                    text = description if stop.reason in ("none", "halted") else stop.reason
                    if stop.reason == "breakpoint":
                        number = next((n for n, b in self.breaks.items() if b.get("sim") == stop.id), None)
                        text = "breakpoint {}".format(number) if number else "breakpoint"
                    elif stop.reason == "watchpoint" and stop.access:
                        kind, addr, _, value, old = stop.access
                        text = "{} {:06X} = {:04X} (was {:04X})".format(kind, addr, value, old)
                finally:
                    self.running = False
                # All talk with Ghidra happens on the executor's one thread,
                # as in Ghidra's own back-ends.
                executor.submit(self.record_stop, text)

            threading.Thread(target=worker, daemon=True).start()

        def step(self, what, description) -> None:
            self.require_stopped()
            what()
            self.record_stop(description)

        def add_break(self, kind: str, address: int, size: int) -> None:
            number = self.next_break
            self.next_break += 1
            b = {"kind": kind, "address": address, "size": max(size, 1), "enabled": True}
            self.breaks[number] = b
            self.arm(number)
            with self.trace.open_tx("Breakpoint"):
                self.put_breakpoint(number)

        def arm(self, number: int) -> None:
            b = self.breaks[number]
            if b["kind"] == "x":
                b["sim"] = self.s.break_at(b["address"])
            else:
                kind = {"R": "read", "W": "write", "RW": "rw"}[b["kind"]]
                b["sim"] = self.s.watch(b["address"], kind, size=b["size"])

        def disarm(self, number: int) -> None:
            b = self.breaks[number]
            if b.get("sim"):
                self.s.delete(b["sim"])
                b["sim"] = None

        def breakpoint_number(self, spec: TraceObject) -> int:
            return int(spec.str_path().rsplit("[", 1)[1].rstrip("]"))

    backend: Dict[str, Backend] = {}

    def B() -> Backend:
        return backend["b"]

    # ---- remote methods (the Debugger's buttons)
    @registry.method(action="resume")
    def resume(process: Process) -> None:
        """Run until a breakpoint, watchpoint or interrupt."""
        B().run_async(lambda: B().s.cont(), "interrupted")

    @registry.method(action="interrupt")
    def interrupt(process: Process) -> None:
        """Stop the simulator."""
        B().s.request_stop()

    @registry.method(action="step_into")
    def step_into(thread: Thread, n: Annotated[int, ParamDesc(display="N")] = 1) -> None:
        """Step one instruction (an EXT or ATOMIC sequence is one step)."""
        def go():
            for _ in range(max(1, n)):
                B().s.step()
        B().step(go, "step")

    @registry.method(action="step_over")
    def step_over(thread: Thread) -> None:
        """Step one instruction, running through calls."""
        B().run_async(lambda: B().s.step_over(), "step over")

    @registry.method(action="step_out")
    def step_out(thread: Thread) -> None:
        """Run until the current routine returns."""
        B().run_async(lambda: B().s.step_out(), "step out")

    @registry.method(action="step_ext", display="Step Back", icon="icon.debugger.step.back.into")
    def step_back_into(thread: Thread) -> None:
        """Step one instruction backwards (the simulator re-runs from a snapshot)."""
        B().step(lambda: B().s.reverse_step(), "step back")

    @registry.method(action="step_ext", display="Resume Back", icon="icon.debugger.resume.back")
    def resume_back(process: Process) -> None:
        """Run backwards to the previous breakpoint or watchpoint hit."""
        B().run_async(lambda: B().s.reverse_continue(), "start of history")

    @registry.method(action="kill", display="Reset")
    def kill(process: Process) -> None:
        """Power-on reset of the simulated CPU (the trace stays open)."""
        B().require_stopped()
        B().s.reset()
        B().record_stop("reset")

    @registry.method(action="break_sw_execute")
    def break_sw_execute(process: Process, address: Address) -> None:
        """Set a breakpoint."""
        B().add_break("x", address.offset, 1)

    @registry.method(action="break_hw_execute")
    def break_hw_execute(process: Process, address: Address) -> None:
        """Set a breakpoint (the simulator has no difference)."""
        B().add_break("x", address.offset, 1)

    @registry.method(action="break_read")
    def break_read(process: Process, range: AddressRange) -> None:
        """Set a read watchpoint."""
        B().add_break("R", range.min, range.length())

    @registry.method(action="break_write")
    def break_write(process: Process, range: AddressRange) -> None:
        """Set a write watchpoint."""
        B().add_break("W", range.min, range.length())

    @registry.method(action="break_access")
    def break_access(process: Process, range: AddressRange) -> None:
        """Set a read/write watchpoint."""
        B().add_break("RW", range.min, range.length())

    @registry.method(action="toggle", display="Toggle Breakpoint")
    def toggle_breakpoint(breakpoint: BreakpointSpec, enabled: bool) -> None:
        """Enable or disable a breakpoint."""
        b = B()
        n = b.breakpoint_number(breakpoint)
        if n not in b.breaks:
            return
        b.breaks[n]["enabled"] = enabled
        if enabled and not b.breaks[n].get("sim"):
            b.arm(n)
        elif not enabled:
            b.disarm(n)
        with b.trace.open_tx("Toggle breakpoint"):
            b.put_breakpoint(n)

    @registry.method(action="delete", display="Delete Breakpoint")
    def delete_breakpoint(breakpoint: BreakpointSpec) -> None:
        """Delete a breakpoint."""
        b = B()
        n = b.breakpoint_number(breakpoint)
        if n in b.breaks:
            b.disarm(n)
            del b.breaks[n]
            with b.trace.open_tx("Delete breakpoint"):
                b.remove_breakpoint(n)

    @registry.method(action="read_mem")
    def read_mem(process: Process, range: AddressRange) -> None:
        """Read simulator memory into the trace."""
        b = B()
        with b.lock, b.trace.open_tx("Read memory"):
            b.put_memory(range.min, range.length())

    @registry.method(action="write_mem")
    def write_mem(process: Process, address: Address, data: bytes) -> None:
        """Write simulator memory (also into ROM images)."""
        b = B()
        b.require_stopped()
        b.s.write(address.offset, data)
        b.s.clear_history()
        with b.lock, b.trace.open_tx("Write memory"):
            b.put_memory(address.offset, len(data))

    @registry.method(action="write_reg")
    def write_reg(frame: StackFrame, name: str, value: bytes) -> None:
        """Write a register."""
        b = B()
        b.require_stopped()
        v = int.from_bytes(value, "big") & 0xFFFF
        if name.lower().startswith("r") and name[1:].isdigit():
            b.s.r[int(name[1:])] = v
        elif name == "IP":
            b.s.pc = (b.s.csp << 16) | v
        elif name == "CSP":
            b.s.pc = ((v & 0xFF) << 16) | b.s.ip
        else:
            b.s.sfr[name] = v
        b.s.clear_history()
        with b.lock, b.trace.open_tx("Write register"):
            b.put_frame()

    @registry.method(action="refresh", display="Refresh Registers")
    def refresh_registers(node: RegisterValueContainer) -> None:
        """Record the registers again."""
        b = B()
        b.require_stopped()
        with b.lock, b.trace.open_tx("Refresh"):
            b.put_frame()

    @registry.method(action="activate")
    def activate_thread(thread: Thread) -> None:
        """There is one CPU."""

    @registry.method(action="activate")
    def activate_frame(frame: StackFrame) -> None:
        """There is one frame."""

    def create(s: simmod.Simulator, program: str, **kw) -> Backend:
        backend["b"] = Backend(s, program, **kw)
        return backend["b"]

    return create, registry


LAUNCHER = r"""#!/usr/bin/env bash
## Written by: python -m openc166.ghidra --install-launcher
#@title openc166 simulator
#@desc <html><body width="300px">
#@desc   <h3>Run in the openc166 simulator</h3>
#@desc   <p>
#@desc     Runs the image in openc166's C166/C167 simulator and debugs it here:
#@desc     breakpoints, watchpoints, stepping, step back and resume back.
#@desc   </p>
#@desc </body></html>
#@menu-group openc166
#@icon icon.debugger
#@depends Debugger-rmi-trace
#@enum Board:str 5wy19 c167cs c167 c166
#@arg :file "Image" "Flash or ROM image (or an openc166 source, .a66)"
#@env OPT_BOARD:Board="5wy19" "Board" "The simulated board"
#@env OPT_PYTHON:file="{python}" "Python" "A Python with the openc166 package"
#@env OPT_SYMBOLS:file="" "Symbols" "Optional: symbols exported with tools/ghidra/ExportOpenc166Symbols.py"
#@env OPT_ADC:str="1=0x2A0" "A/D inputs" "Space-separated CH=VALUE pairs (the 5WY19 boot block wants channel 1 above 0C4H)"
#@env OPT_BOOT_MODE:bool=false "Boot mode" "5WY19: erase the application signature so the boot block runs"
#@env OPT_FREEZE_WATCHDOG:bool=false "Freeze watchdog" "The watchdog never resets the CPU"

args=("$1" --board "$OPT_BOARD")
[ -n "$OPT_SYMBOLS" ] && args+=(--symbols "$OPT_SYMBOLS")
for adc in $OPT_ADC; do args+=(--adc "$adc"); done
[ "$OPT_BOOT_MODE" = "true" ] && args+=(--boot-mode)
[ "$OPT_FREEZE_WATCHDOG" = "true" ] && args+=(--freeze-watchdog)
exec "$OPT_PYTHON" -m openc166.ghidra "${{args[@]}}"
"""


def ghidra_settings_dir(ghidra: str) -> str:
    """The user settings directory of a Ghidra install (11.0 and later)."""
    props = {}
    with open(os.path.join(ghidra, "Ghidra", "application.properties"), encoding="utf-8") as f:
        for line in f:
            if "=" in line and not line.startswith("#"):
                k, _, v = line.strip().partition("=")
                props[k] = v
    name = "ghidra_{}_{}".format(props["application.version"], props.get("application.release.name", "PUBLIC"))
    if sys.platform == "win32":
        base = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "ghidra")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/ghidra")
    else:
        base = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "ghidra")
    return os.path.join(base, name), props["application.version"]


def install_launcher(ghidra: str, extensions_dir: Optional[str] = None) -> str:
    """Writes a small Ghidra extension holding the openc166 launcher into the
    user's Extensions directory of that Ghidra. Returns its path."""
    settings, version = ghidra_settings_dir(ghidra)
    root = os.path.join(extensions_dir or os.path.join(settings, "Extensions"), "openc166-debugger")
    os.makedirs(os.path.join(root, "data", "debugger-launchers"), exist_ok=True)
    with open(os.path.join(root, "extension.properties"), "w", encoding="utf-8") as f:
        f.write("name=openc166 debugger\n"
                "description=Debug C166/C167 firmware in the openc166 simulator\n"
                "author=openc166\n"
                "createdOn=\n"
                "version={}\n".format(version))
    open(os.path.join(root, "Module.manifest"), "w").close()
    path = os.path.join(root, "data", "debugger-launchers", "openc166.sh")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(LAUNCHER.format(python=sys.executable))
    os.chmod(path, 0o755)
    return root


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(prog="python -m openc166.ghidra", description=__doc__.split("\n\n")[0])
    p.add_argument("image", nargs="?", help="flash or ROM image (or a .a66 source to assemble)")
    p.add_argument("--install-launcher", metavar="GHIDRA_DIR",
                   help="install the 'openc166 simulator' launcher into this Ghidra's user extensions and exit")
    p.add_argument("--extensions-dir", help="with --install-launcher: write the extension here instead")
    p.add_argument("--board", default="c167", choices=simmod.Simulator.BOARDS)
    g = p.add_mutually_exclusive_group()
    g.add_argument("--connect", help="Ghidra's Trace RMI address host:port (default: $GHIDRA_TRACE_RMI_ADDR)")
    g.add_argument("--listen", help="wait for Ghidra to connect at [host:]port")
    p.add_argument("--program", help="name of the Ghidra program to map onto (default: the image's file name)")
    p.add_argument("--ghidra", help="Ghidra install directory, to find its Python client")
    p.add_argument("--symbols", help="Ghidra export (JSON or CSV), for frame names")
    p.add_argument("--debug-info", help="openc166 debug information (JSON)")
    p.add_argument("--adc", action="append", default=[], metavar="CH=VALUE")
    p.add_argument("--boot-mode", action="store_true", help="5WY19: erase the application signature")
    p.add_argument("--freeze-watchdog", action="store_true")
    a = p.parse_args(argv)

    if a.install_launcher:
        root = install_launcher(a.install_launcher, a.extensions_dir)
        print("installed {}".format(root))
        print("restart Ghidra; in the Debugger the launcher is under 'openc166 simulator'")
        return 0
    if not a.image:
        p.error("the image is required")
    gt = import_ghidratrace(a.ghidra)
    from ghidratrace import client as gt_client

    if a.image.lower().endswith((".a66", ".asm", ".src")):
        import openc166

        r = openc166.assemble_file(a.image)
        for d in r.diagnostics:
            print(d, file=sys.stderr)
        if not r.ok:
            return 1
        s = simmod.Simulator.from_result(r, a.board)
    else:
        with open(a.image, "rb") as f:
            data = f.read()
        if a.boot_mode:
            data = bytearray(data)
            data[0x1FFF0:0x1FFF4] = b"\xff" * 4
            data = bytes(data)
        s = simmod.Simulator(data, a.board)
    if a.symbols:
        s.symbols.load_ghidra(a.symbols)
    if a.debug_info:
        s.symbols.load_debug_info(a.debug_info)
    for item in a.adc:
        ch, _, value = item.partition("=")
        s.set_adc(int(ch), simmod._int(value))
    s.watchdog_frozen = a.freeze_watchdog

    create, _ = make_backend(gt_client)
    program = a.program or os.path.basename(a.image)
    b = create(s, program)
    if a.listen:
        b.listen(a.listen)
    else:
        address = a.connect or os.getenv("GHIDRA_TRACE_RMI_ADDR")
        if not address:
            p.error("give --connect host:port or --listen, or run from Ghidra's launcher")
        b.connect(address)
    b.start("openc166/" + program)
    print("connected to {}; tracing {}".format(b.client.description, program), flush=True)
    del gt
    try:
        b.client.receiver.join()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
