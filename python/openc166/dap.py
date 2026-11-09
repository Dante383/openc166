# SPDX-License-Identifier: MIT
"""Debug Adapter Protocol server for the openc166 simulator.

Editors talk to it over stdin and stdout:

    python -m openc166.dap

Launch arguments (the "launch" request, e.g. in VS Code's launch.json):

    image            binary to load as flash or ROM from address 0
    program          or: an openc166 source to assemble (with debug info)
    board            "5wy19", "c167cs", "c167" (default) or "c166"
    debugInfo        openc166 debug information (JSON from openc166 -g)
    debugInfoOffset  added to its addresses (e.g. 0x80000 for a mirror)
    ghidraSymbols    a Ghidra export (tools/ghidra/ExportOpenc166Symbols.py)
    includeDirs      for "program": more $INCLUDE directories
    stopOnEntry      stop before the first instruction (default true)
    watchdogFrozen   the watchdog never resets the CPU (default false)
    stopOnUnmodelled stop when firmware touches an unmodelled register

The debug console evaluates register and SFR names, symbols, "[addr]"
(a word) and, failing those, Python with 's' bound to the Simulator.
"""

from __future__ import annotations

import base64
import json
import os
import sys
import threading
import traceback
from typing import Any, Dict, List, Optional

import openc166

from . import sim as simmod

THREAD_ID = 1

# Variable references of the fixed scopes.
REF_REGISTERS, REF_FLAGS, REF_SYSTEM, REF_EXEC = 1, 2, 3, 4

CORE_SFRS = ["IP", "CSP", "PSW", "SP", "CP", "DPP0", "DPP1", "DPP2", "DPP3", "MDH", "MDL", "MDC", "STKOV", "STKUN"]
SYSTEM_SFRS = [
    "SYSCON", "BUSCON0", "BUSCON1", "BUSCON2", "BUSCON3", "BUSCON4", "ADDRSEL1", "ADDRSEL2", "ADDRSEL3",
    "ADDRSEL4", "XPERCON", "TFR", "WDTCON",
]
FLAGS = [("N", 0), ("C", 1), ("V", 2), ("Z", 3), ("E", 4), ("MULIP", 5), ("USR0", 6), ("IEN", 11)]


class Server:
    def __init__(self, inp=None, out=None):
        self.inp = inp or sys.stdin.buffer
        self.out = out or sys.stdout.buffer
        self.seq = 1
        self.lock = threading.Lock()
        self.s: Optional[simmod.Simulator] = None
        self.running = False
        self.source_bps: Dict[str, List[int]] = {}
        self.function_bps: List[int] = []
        self.instruction_bps: List[int] = []
        self.data_bps: List[int] = []
        self.stop_on_entry = True
        self.done = False

    # ---- transport
    def send(self, msg: Dict[str, Any]) -> None:
        with self.lock:
            msg["seq"] = self.seq
            self.seq += 1
            body = json.dumps(msg).encode("utf-8")
            self.out.write(b"Content-Length: %d\r\n\r\n" % len(body))
            self.out.write(body)
            self.out.flush()

    def event(self, name: str, body: Optional[dict] = None) -> None:
        self.send({"type": "event", "event": name, "body": body or {}})

    def respond(self, req: dict, body: Optional[dict] = None, success: bool = True, message: str = "") -> None:
        m = {"type": "response", "request_seq": req["seq"], "command": req["command"], "success": success}
        if body is not None:
            m["body"] = body
        if message:
            m["message"] = message
        self.send(m)

    def output(self, text: str, category: str = "console") -> None:
        self.event("output", {"category": category, "output": text if text.endswith("\n") else text + "\n"})

    def read_message(self) -> Optional[dict]:
        length = None
        while True:
            line = self.inp.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
        if length is None:
            return None
        return json.loads(self.inp.read(length).decode("utf-8"))

    def serve(self) -> None:
        while not self.done:
            msg = self.read_message()
            if msg is None:
                break
            if msg.get("type") != "request":
                continue
            cmd = msg["command"]
            handler = getattr(self, "on_" + cmd, None)
            if handler is None:
                self.respond(msg, success=False, message="unsupported request " + cmd)
                continue
            if self.running and cmd not in ("pause", "threads", "disconnect", "terminate"):
                self.respond(msg, success=False, message="the target is running")
                continue
            try:
                handler(msg, msg.get("arguments") or {})
            except Exception as e:  # report, keep serving
                self.output(traceback.format_exc(), "stderr")
                self.respond(msg, success=False, message=str(e))

    # ---- session
    def on_initialize(self, req, args):
        self.respond(req, {
            "supportsConfigurationDoneRequest": True,
            "supportsStepBack": True,
            "supportsDisassembleRequest": True,
            "supportsReadMemoryRequest": True,
            "supportsWriteMemoryRequest": True,
            "supportsInstructionBreakpoints": True,
            "supportsFunctionBreakpoints": True,
            "supportsConditionalBreakpoints": True,
            "supportsHitConditionalBreakpoints": False,
            "supportsDataBreakpoints": True,
            "supportsSetVariable": True,
            "supportsEvaluateForHovers": True,
            "supportsSteppingGranularity": True,
            "supportsTerminateRequest": True,
            "supportsGotoTargetsRequest": False,
        })

    def on_launch(self, req, args):
        board = args.get("board", "c167")
        symbols = simmod.Symbols()
        if args.get("program"):
            path = args["program"]
            r = openc166.assemble_file(path, include_dirs=args.get("includeDirs", []))
            for d in r.diagnostics:
                self.output(str(d), "stderr" if d.is_error else "console")
            if not r.ok:
                self.respond(req, success=False, message="assembly failed")
                return
            self.s = simmod.Simulator.from_result(r, board)
        elif args.get("image"):
            self.s = simmod.Simulator.from_file(args["image"], board, symbols)
        else:
            self.respond(req, success=False, message="launch needs 'image' or 'program'")
            return
        if args.get("debugInfo"):
            self.s.symbols.load_debug_info(args["debugInfo"], int(args.get("debugInfoOffset", 0)))
        if args.get("ghidraSymbols"):
            self.s.symbols.load_ghidra(args["ghidraSymbols"])
        self.s.watchdog_frozen = bool(args.get("watchdogFrozen", False))
        self.s.stop_on_unmodelled = bool(args.get("stopOnUnmodelled", False))
        self.stop_on_entry = bool(args.get("stopOnEntry", True))
        self.respond(req)
        self.event("initialized")

    on_attach = on_launch

    def on_configurationDone(self, req, args):
        self.respond(req)
        if self.stop_on_entry:
            self.event("stopped", {"reason": "entry", "threadId": THREAD_ID, "allThreadsStopped": True})
        else:
            self.run_async(lambda: self.s.cont())

    def on_disconnect(self, req, args):
        if self.s and self.running:
            self.s.request_stop()
        self.respond(req)
        self.done = True

    def on_terminate(self, req, args):
        self.on_disconnect(req, args)
        self.event("terminated")

    def on_threads(self, req, args):
        self.respond(req, {"threads": [{"id": THREAD_ID, "name": "CPU"}]})

    # ---- breakpoints
    def _condition(self, text: Optional[str]):
        if not text:
            return None
        code = compile(text, "<condition>", "eval")
        return lambda s: bool(eval(code, self.namespace()))

    def on_setBreakpoints(self, req, args):
        path = args["source"].get("path") or args["source"].get("name", "")
        for bid in self.source_bps.pop(path, []):
            self.s.delete(bid)
        result, ids = [], []
        for bp in args.get("breakpoints", []):
            a = self.s.symbols.address_of_line(path, bp["line"])
            if a is None:
                result.append({"verified": False, "line": bp["line"], "message": "no code at this line"})
                continue
            bid = self.s.break_at(a, self._condition(bp.get("condition")))
            ids.append(bid)
            line = self.s.symbols.line_at(a)
            result.append({"id": bid, "verified": True, "line": line.line if line else bp["line"],
                           "instructionReference": hex(a)})
        self.source_bps[path] = ids
        self.respond(req, {"breakpoints": result})

    def on_setFunctionBreakpoints(self, req, args):
        for bid in self.function_bps:
            self.s.delete(bid)
        self.function_bps = []
        result = []
        for bp in args.get("breakpoints", []):
            try:
                a = self.s.resolve(bp["name"])
            except KeyError:
                result.append({"verified": False, "message": "unknown name"})
                continue
            bid = self.s.break_at(a, self._condition(bp.get("condition")))
            self.function_bps.append(bid)
            result.append({"id": bid, "verified": True, "instructionReference": hex(a)})
        self.respond(req, {"breakpoints": result})

    def on_setInstructionBreakpoints(self, req, args):
        for bid in self.instruction_bps:
            self.s.delete(bid)
        self.instruction_bps = []
        result = []
        for bp in args.get("breakpoints", []):
            a = int(bp["instructionReference"], 0) + int(bp.get("offset", 0))
            bid = self.s.break_at(a, self._condition(bp.get("condition")))
            self.instruction_bps.append(bid)
            result.append({"id": bid, "verified": True, "instructionReference": hex(a)})
        self.respond(req, {"breakpoints": result})

    def _data_target(self, name: str) -> Optional[str]:
        """A dataId for a register, SFR, bit, symbol or address."""
        n = name.strip()
        if n.upper().startswith("R") and n[1:].isdigit():
            return None  # GPRs move with CP; watch their memory instead
        if simmod.sfr_address(n) is not None or simmod._bit(n) is not None or n in self.s.symbols.by_name:
            return n
        try:
            return hex(self.s.resolve(n))
        except KeyError:
            return None

    def on_dataBreakpointInfo(self, req, args):
        target = self._data_target(args.get("name", ""))
        if target is None:
            self.respond(req, {"dataId": None, "description": "cannot watch this"})
            return
        self.respond(req, {"dataId": target, "description": target,
                           "accessTypes": ["read", "write", "readWrite"], "canPersist": True})

    def on_setDataBreakpoints(self, req, args):
        for bid in self.data_bps:
            self.s.delete(bid)
        self.data_bps = []
        kinds = {"read": "read", "write": "write", "readWrite": "rw"}
        result = []
        for bp in args.get("breakpoints", []):
            target = bp["dataId"]
            loc = int(target, 0) if target.startswith("0x") else target
            kind = kinds.get(bp.get("accessType", "write"), "write")
            # A single bit: a write that leaves it alone is not interesting.
            if kind == "write" and isinstance(loc, str) and simmod.sfr_address(loc) is None and simmod._bit(loc):
                kind = "change"
            bid = self.s.watch(loc, kind,
                               condition=self._condition(bp.get("condition")))
            self.data_bps.append(bid)
            result.append({"id": bid, "verified": True})
        self.respond(req, {"breakpoints": result})

    # ---- execution
    def run_async(self, fn, reason_override: Optional[str] = None) -> None:
        self.running = True
        before = len(self.s.unmodelled)

        def worker():
            try:
                stop = fn()
            except Exception as e:
                self.output("simulator error: {}".format(e), "stderr")
                stop = simmod.Stop("halted")
            self.running = False
            self.report_stop(stop, reason_override, before)

        threading.Thread(target=worker, daemon=True).start()

    def report_stop(self, stop: simmod.Stop, reason_override: Optional[str], unmodelled_before: int) -> None:
        body = {"threadId": THREAD_ID, "allThreadsStopped": True}
        if stop.reason == "breakpoint":
            body.update(reason="breakpoint", hitBreakpointIds=[stop.id])
        elif stop.reason == "watchpoint":
            body.update(reason="data breakpoint", hitBreakpointIds=[stop.id])
            if stop.access:
                kind, addr, size, value, old = stop.access
                body["description"] = "{} {:06X}H = {:04X}H (was {:04X}H)".format(kind, addr, value, old)
        elif stop.reason == "unmodelled":
            u = self.s.unmodelled[unmodelled_before:] or self.s.unmodelled[-1:]
            body.update(reason="exception", description="unmodelled access",
                        text="; ".join("{} {} at {:06X}".format("write" if x.write else "read", x.what, x.pc) for x in u))
        elif stop.history_start:
            body.update(reason="step", description="start of recorded history")
        elif stop.reason in ("idle", "powerdown", "undefined"):
            body.update(reason="pause", description=stop.reason)
        else:
            body.update(reason=reason_override or "pause")
        new = self.s.unmodelled[unmodelled_before:]
        for u in new[:20]:
            self.output("unmodelled: {} {} at pc {:06X} (instruction {})".format(
                "write" if u.write else "read", u.what, u.pc, u.instruction))
        self.event("stopped", body)

    def on_continue(self, req, args):
        self.respond(req, {"allThreadsContinued": True})
        self.run_async(lambda: self.s.cont())

    def on_next(self, req, args):
        self.respond(req)
        self.run_async(lambda: self.s.step_over(), "step")

    def on_stepIn(self, req, args):
        self.respond(req)
        self.run_async(lambda: self.s.step(), "step")

    def on_stepOut(self, req, args):
        self.respond(req)
        self.run_async(lambda: self.s.step_out(), "step")

    def on_stepBack(self, req, args):
        self.respond(req)
        self.run_async(lambda: self.s.reverse_step(), "step")

    def on_reverseContinue(self, req, args):
        self.respond(req)
        self.run_async(lambda: self.s.reverse_continue(), "step")

    def on_pause(self, req, args):
        if self.running:
            self.s.request_stop()
        self.respond(req)

    # ---- state
    def on_stackTrace(self, req, args):
        pc = self.s.pc
        frame = {"id": 1, "name": self.s.symbols.describe(pc), "line": 0, "column": 0,
                 "instructionPointerReference": hex(pc)}
        line = self.s.symbols.line_at(pc)
        if line:
            frame.update(source={"name": os.path.basename(line.file), "path": os.path.abspath(line.file)},
                         line=line.line, column=1)
        else:
            frame["presentationHint"] = "subtle"
        self.respond(req, {"stackFrames": [frame], "totalFrames": 1})

    def on_scopes(self, req, args):
        self.respond(req, {"scopes": [
            {"name": "Registers", "variablesReference": REF_REGISTERS, "expensive": False, "presentationHint": "registers"},
            {"name": "PSW", "variablesReference": REF_FLAGS, "expensive": False},
            {"name": "System", "variablesReference": REF_SYSTEM, "expensive": False},
            {"name": "Execution", "variablesReference": REF_EXEC, "expensive": False},
        ]})

    def _var(self, name, value, width=4, ref=0, mem=None):
        v = {"name": name, "value": "{:0{}X}H".format(value, width) if isinstance(value, int) else str(value),
             "variablesReference": ref}
        if mem is not None:
            v["memoryReference"] = hex(mem)
        return v

    def on_variables(self, req, args):
        ref = args["variablesReference"]
        s = self.s
        out = []
        if ref == REF_REGISTERS:
            cp = s.sfr["CP"]
            for i in range(16):
                out.append(self._var("R{}".format(i), s.r[i], mem=cp + 2 * i))
            out.append(self._var("IP", s.ip))
            for n in CORE_SFRS[1:]:
                out.append(self._var(n, s.sfr[n], mem=simmod.sfr_address(n)))
        elif ref == REF_FLAGS:
            psw = s.psw
            for n, b in FLAGS:
                out.append({"name": n, "value": str((psw >> b) & 1), "variablesReference": 0})
            out.append({"name": "ILVL", "value": str(psw >> 12), "variablesReference": 0})
        elif ref == REF_SYSTEM:
            for n in SYSTEM_SFRS:
                a = simmod.sfr_address(n)
                if a is not None:
                    out.append(self._var(n, s.sfr[a], mem=a))
            for i in range(4):
                dpp = s.sfr["DPP{}".format(i)]
                out.append({"name": "page {} (DPP{})".format(i, i),
                            "value": "{:04X}H..{:04X}H -> {:06X}H".format(i << 14, (i << 14) | 0x3FFF, (dpp & 0x3FF) << 14),
                            "variablesReference": 0})
        elif ref == REF_EXEC:
            ext = s.ext
            out += [
                {"name": "instructions", "value": str(s.instructions), "variablesReference": 0},
                {"name": "cycles", "value": str(s.cycles), "variablesReference": 0},
                {"name": "time", "value": "{:.6f} s".format(s.time), "variablesReference": 0},
                {"name": "history from", "value": str(s.history_start), "variablesReference": 0},
                {"name": "EINIT done", "value": str(s.einit_done), "variablesReference": 0},
                {"name": "EXT sequence", "value": "none" if not ext["left"] else
                    "{} left, {}{}".format(ext["left"], ext["kind"], ", ESFR" if ext["esfr"] else ""),
                 "variablesReference": 0},
                {"name": "unmodelled accesses", "value": str(len(s.unmodelled)), "variablesReference": 0},
            ]
        self.respond(req, {"variables": out})

    def on_setVariable(self, req, args):
        name, text = args["name"], args["value"].strip()
        v = simmod._addr(text) if text.upper().endswith("H") or text.lower().startswith("0x") else int(text, 0)
        s = self.s
        if name.upper().startswith("R") and name[1:].isdigit():
            s.r[int(name[1:])] = v
        elif name == "IP":
            s.pc = (s.csp << 16) | (v & 0xFFFF)
        elif name in [n for n, _ in FLAGS]:
            bit = dict(FLAGS)[name]
            s.sfr["PSW"] = (s.psw & ~(1 << bit)) | ((v & 1) << bit)
        elif simmod.sfr_address(name) is not None:
            s.sfr[name] = v
        else:
            raise ValueError("cannot set " + name)
        s.clear_history()
        self.respond(req, {"value": "{:04X}H".format(v & 0xFFFF)})

    def namespace(self) -> dict:
        s = self.s
        return {"s": s, "sim": s, "r": s.r, "sfr": s.sfr, "read": s.read, "read16": s.read16, "hex": hex}

    def on_evaluate(self, req, args):
        expr = args["expression"].strip()
        s = self.s
        up = expr.upper()
        result: Any = None
        mem = None
        if up.startswith("R") and up[1:].isdigit() and int(up[1:]) < 16:
            result = "{:04X}H".format(s.r[int(up[1:])])
        elif up in ("IP", "PC"):
            result = "{:06X}H".format(s.pc)
        elif simmod.sfr_address(expr) is not None:
            mem = simmod.sfr_address(expr)
            result = "{:04X}H".format(s.sfr[expr])
        elif expr in s.symbols.by_name:
            mem = s.symbols.by_name[expr]
            result = "{:06X}H: {:04X}H".format(mem, s.read16(mem))
        elif expr.startswith("[") and expr.endswith("]"):
            mem = s.resolve(expr[1:-1].strip())
            result = "{:04X}H".format(s.read16(mem))
        else:
            try:
                v = eval(expr, self.namespace())
            except SyntaxError:
                exec(expr, self.namespace())
                v = None
            result = "" if v is None else ("{:X}H ({})".format(v, v) if isinstance(v, int) else repr(v))
        body = {"result": result, "variablesReference": 0}
        if mem is not None:
            body["memoryReference"] = hex(mem)
        self.respond(req, body)

    # ---- memory and code
    def on_readMemory(self, req, args):
        a = int(args["memoryReference"], 0) + int(args.get("offset", 0))
        n = int(args["count"])
        data = self.s.read(a & 0xFFFFFF, n)
        self.respond(req, {"address": hex(a), "data": base64.b64encode(data).decode("ascii")})

    def on_writeMemory(self, req, args):
        a = int(args["memoryReference"], 0) + int(args.get("offset", 0))
        data = base64.b64decode(args["data"])
        self.s.write(a & 0xFFFFFF, data)
        self.s.clear_history()
        self.respond(req, {"bytesWritten": len(data)})

    def on_disassemble(self, req, args):
        ref = int(args["memoryReference"], 0) + int(args.get("offset", 0))
        offset = int(args.get("instructionOffset", 0))
        count = int(args["instructionCount"])
        out: List[dict] = []
        if offset < 0:
            # Instructions are 2 or 4 bytes: decode forwards from far enough
            # back and keep the ones that end at 'ref'.
            start = ref + 4 * offset
            before = []
            a = start
            while a < ref:
                ins = self.s.disassemble(a, 1)[0]
                before.append(ins)
                a += ins.size
            if a != ref:  # landed inside an instruction: try 2 bytes later
                before, a = [], start + 2
                while a < ref:
                    ins = self.s.disassemble(a, 1)[0]
                    before.append(ins)
                    a += ins.size
            before = before[offset:] if len(before) >= -offset else before
            out += [self._insn(i) for i in before]
            missing = -offset - len(before)
            out = [{"address": hex(max(ref - 2 * (missing - k), 0)), "instruction": "??", "presentationHint": "invalid"}
                   for k in range(missing)] + out
            count -= len(out)
            a = ref
        else:
            a = ref
            for ins in self.s.disassemble(a, offset):
                a += ins.size
        if count > 0:
            out += [self._insn(i) for i in self.s.disassemble(a, count)]
        self.respond(req, {"instructions": out[: int(args["instructionCount"])]})

    def _insn(self, ins: simmod.Instruction) -> dict:
        d = {"address": hex(ins.address), "instructionBytes": ins.data.hex(" "), "instruction": ins.text}
        name = self.s.symbols.name_at(ins.address)
        if name:
            d["symbol"] = name
        line = self.s.symbols.line_at(ins.address)
        if line:
            d["location"] = {"name": os.path.basename(line.file), "path": os.path.abspath(line.file)}
            d["line"] = line.line
        return d


def main(argv=None) -> int:
    Server().serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
