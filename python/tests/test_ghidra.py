# SPDX-License-Identifier: MIT
"""The Ghidra back-end (openc166.ghidra) against a stand-in for Ghidra.

The stand-in speaks Trace RMI with Ghidra's own protobuf definitions:
it answers every request, records the objects, values, registers, memory
and snapshots the back-end writes, and invokes methods the way the
Debugger's buttons do. Ghidra's client library comes from the Ghidra
install named by GHIDRA_INSTALL_DIR; without it the tests are skipped.
"""

import os
import socket
import threading
import time
import unittest

import openc166
from openc166 import sim

GHIDRA = os.environ.get("GHIDRA_INSTALL_DIR")

PROGRAM = """$MOD167
CODE0   SECTION CODE AT 0
START:  DISWDT
        EINIT
        MOV     R4, #0
LOOP:   ADD     R4, #1
        MOV     0FC40H, R4
AFTER:  CMP     R4, #100
        JMPR    cc_NE, LOOP
DONE:   JMPR    cc_UC, DONE
CODE0   ENDS
        END
"""


def have_ghidratrace():
    if not GHIDRA:
        return False
    try:
        from openc166 import ghidra

        ghidra.import_ghidratrace(GHIDRA)
        return True
    except (SystemExit, Exception):  # e.g. a protobuf too old for that Ghidra's client
        return False


class FakeGhidra:
    """Enough of Ghidra's Trace RMI server to drive a back-end."""

    def __init__(self):
        from ghidratrace import trace_rmi_pb2 as bufs
        from ghidratrace import util

        self.bufs, self.util = bufs, util
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.address = "127.0.0.1:{}".format(self.server.getsockname()[1])
        self.ids = {}
        self.paths = {}
        self.values = {}  # (path, key) -> value
        self.registers = {}  # name -> int (latest)
        self.memory = {}  # address -> byte (latest)
        self.snapshots = []
        self.methods = {}
        self.slock = threading.Lock()
        self.replies = {}
        self.reply_event = threading.Condition()
        self.seq = 0
        threading.Thread(target=self.serve, daemon=True).start()

    # ---- helpers
    def obj_id(self, path):
        if path not in self.ids:
            self.ids[path] = len(self.ids) + 1
            self.paths[self.ids[path]] = path
        return self.ids[path]

    def spec_path(self, spec):
        if spec.HasField("path") and spec.path.path:
            return spec.path.path
        return self.paths.get(spec.id, "?")

    def decode(self, v):
        kind = v.WhichOneof("value")
        if kind is None or kind == "null_value":
            return None
        if kind == "address_value":
            return ("addr", v.address_value.space, v.address_value.offset)
        if kind == "range_value":
            return ("range", v.range_value.space, v.range_value.offset, v.range_value.offset + v.range_value.extend)
        if kind in ("child_spec", "child_desc"):
            return ("obj", self.spec_path(getattr(v, kind)))
        return getattr(v, kind)

    def send(self, root):
        with self.slock:
            self.util.send_delimited(self.conn, root)

    def get(self, path, key):
        return self.values.get((path, key))

    # ---- the server
    def serve(self):
        self.conn, _ = self.server.accept()
        self.conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        b = self.bufs
        while True:
            root = b.RootMessage()
            try:
                self.util.recv_delimited(self.conn, root, self.seq)
            except Exception:
                return
            self.seq += 1
            kind = root.WhichOneof("msg")
            reply = b.RootMessage()
            if kind == "xreply_invoke_method":
                with self.reply_event:
                    self.replies[len(self.replies)] = root.xreply_invoke_method
                    self.reply_event.notify_all()
                continue
            req = getattr(root, kind)
            if kind == "request_negotiate":
                for m in req.methods:
                    self.methods[m.name] = m
                reply.reply_negotiate.description = "fake Ghidra"
            elif kind == "request_create_root_object":
                # (ObjSpec's id and path are a oneof: reply with the id.)
                reply.reply_create_object.object.id = self.obj_id("")
            elif kind == "request_create_object":
                path = req.path.path
                reply.reply_create_object.object.id = self.obj_id(path)
            elif kind == "request_insert_object":
                reply.reply_insert_object.span.min = 0
                reply.reply_insert_object.span.max = 1 << 62
            elif kind == "request_set_value":
                path = self.spec_path(req.value.parent)
                self.values[(path, req.value.key)] = self.decode(req.value.value)
                reply.reply_set_value.span.min = 0
                reply.reply_set_value.span.max = 1 << 62
            elif kind == "request_put_register_value":
                for rv in req.values:
                    self.registers[rv.name] = int.from_bytes(rv.value, "big")
                reply.reply_put_register_value.SetInParent()
            elif kind == "request_put_bytes":
                for i, byte in enumerate(req.data):
                    self.memory[req.start.offset + i] = byte
                reply.reply_put_bytes.written = len(req.data)
            elif kind == "request_snapshot":
                self.snapshots.append(req.description)
                reply.reply_snapshot.snap.snap = req.snap.snap
            else:
                getattr(reply, "reply_" + kind[len("request_"):]).SetInParent()
            self.send(reply)

    def invoke(self, method, timeout=20, **args):
        """Calls a back-end method as a Debugger button does."""
        b = self.bufs
        root = b.RootMessage()
        root.xrequest_invoke_method.oid.id = 1
        root.xrequest_invoke_method.name = method
        for k, v in args.items():
            arg = root.xrequest_invoke_method.arguments.add()
            arg.name = k
            if isinstance(v, tuple) and v[0] == "obj":
                arg.value.child_desc.id = self.obj_id(v[1])
                arg.value.child_desc.path.path = v[1]  # ObjDesc has both
            elif isinstance(v, tuple) and v[0] == "addr":
                arg.value.address_value.space = "ram"
                arg.value.address_value.offset = v[1]
            elif isinstance(v, tuple) and v[0] == "range":
                arg.value.range_value.space = "ram"
                arg.value.range_value.offset = v[1]
                arg.value.range_value.extend = v[2] - 1
            elif isinstance(v, bool):
                arg.value.bool_value = v
            elif isinstance(v, int):
                arg.value.long_value = v
            elif isinstance(v, bytes):
                arg.value.bytes_value = v
            else:
                arg.value.string_value = v
        with self.reply_event:
            n = len(self.replies)
            self.send(root)
            self.reply_event.wait_for(lambda: len(self.replies) > n, timeout)
            reply = self.replies[n]
        if reply.error:
            raise AssertionError("{} failed: {}".format(method, reply.error))

    def wait_state(self, state, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.get("Processes[0]", "_state") == state:
                return
            time.sleep(0.02)
        raise AssertionError("state is not {}".format(state))


@unittest.skipUnless(have_ghidratrace(), "set GHIDRA_INSTALL_DIR to a Ghidra with Trace RMI (10.3 or later)")
class GhidraBackendTest(unittest.TestCase):
    def setUp(self):
        from ghidratrace import client as gt_client
        from openc166 import ghidra

        r = openc166.assemble(PROGRAM, file_name="prog.a66")
        self.assertTrue(r.ok, r.errors)
        self.s = sim.Simulator.from_result(r)
        self.g = FakeGhidra()
        create, _ = ghidra.make_backend(gt_client)
        self.b = create(self.s, "prog.bin")
        self.b.connect(self.g.address)
        self.b.start("openc166/prog")

    def tearDown(self):
        self.b.client.close()
        self.s.close()

    def test_initial_trace(self):
        g = self.g
        self.assertIn("ghidra_trace", "ghidra_trace")
        self.assertEqual(g.get("Processes[0]", "_state"), "STOPPED")
        self.assertEqual(g.get("Processes[0].Environment", "_arch"), "C166")
        self.assertEqual(g.registers["IP"], 0)
        self.assertEqual(g.registers["r0"], self.s.r[0])
        self.assertEqual(g.get("Processes[0].Threads[0].Stack[0]", "_pc"), ("addr", "ram", 0))
        # Internal RAM and SFRs are recorded at every stop.
        self.assertEqual(g.memory[0xFE10], 0x00)  # CP low byte
        self.assertEqual(g.memory[0xFE11], 0xFC)
        regions = {p for (p, k) in g.values if p.startswith("Processes[0].Memory[") and k == "_range"}
        self.assertTrue(any("00f600" in r or "00f200" in r for r in regions), regions)
        self.assertEqual(g.get('Processes[0].Modules["prog.bin"]', "_module_name"), "prog.bin")
        for m in ("resume", "interrupt", "step_into", "step_over", "step_out", "break_sw_execute",
                  "break_write", "toggle_breakpoint", "delete_breakpoint", "read_mem", "write_mem", "write_reg"):
            self.assertIn(m, g.methods)
        self.assertEqual(g.methods["step_back_into"].action, "step_ext")

    def test_breakpoint_resume_step_and_back(self):
        g, s = self.g, self.s
        after = s.symbols.by_name["AFTER"]
        g.invoke("break_sw_execute", process=("obj", "Processes[0]"), address=("addr", after))
        self.assertEqual(g.get("Breakpoints[1]", "_kinds"), "SW_EXECUTE")
        self.assertEqual(g.get("Breakpoints[1][1]", "_range"), ("range", "ram", after, after))
        g.invoke("resume", process=("obj", "Processes[0]"))
        g.wait_state("STOPPED")
        time.sleep(0.2)
        self.assertEqual(s.pc, after)
        self.assertEqual(g.get("Processes[0].Threads[0].Stack[0]", "_pc"), ("addr", "ram", after))
        self.assertIn("breakpoint 1", g.snapshots[-1])
        self.assertEqual(g.registers["r4"], 1)

        n = s.instructions
        g.invoke("step_into", thread=("obj", "Processes[0].Threads[0]"))
        self.assertEqual(s.instructions, n + 1)
        self.assertEqual(g.registers["IP"], s.ip)
        g.invoke("step_back_into", thread=("obj", "Processes[0].Threads[0]"))
        self.assertEqual(s.instructions, n)
        self.assertEqual(g.registers["IP"], after)

        g.invoke("toggle_breakpoint", breakpoint=("obj", "Breakpoints[1]"), enabled=False)
        self.assertEqual(g.get("Breakpoints[1]", "_enabled"), False)
        g.invoke("resume", process=("obj", "Processes[0]"))
        time.sleep(0.3)
        g.invoke("interrupt", process=("obj", "Processes[0]"))
        g.wait_state("STOPPED")
        time.sleep(0.2)
        self.assertEqual(s.symbols.name_at(s.pc), "DONE")  # ran past AFTER to the end
        g.invoke("delete_breakpoint", breakpoint=("obj", "Breakpoints[1]"))

    def test_watchpoint_registers_and_memory(self):
        g, s = self.g, self.s
        g.invoke("break_write", process=("obj", "Processes[0]"), range=("range", 0xFC40, 2))
        g.invoke("resume", process=("obj", "Processes[0]"))
        g.wait_state("STOPPED")
        time.sleep(0.2)
        self.assertIn("write 00FC40 = 0001", g.snapshots[-1])
        g.invoke("write_reg", frame=("obj", "Processes[0].Threads[0].Stack[0]"), name="r4", value=b"\x12\x34")
        self.assertEqual(s.r[4], 0x1234)
        self.assertEqual(g.registers["r4"], 0x1234)
        g.invoke("write_mem", process=("obj", "Processes[0]"), address=("addr", 0xFC60), data=b"\xAA\x55")
        self.assertEqual(s.read(0xFC60, 2), b"\xAA\x55")
        g.invoke("read_mem", process=("obj", "Processes[0]"), range=("range", 0x0000, 16))
        self.assertEqual(bytes(g.memory[i] for i in range(16)), s.read(0, 16))


if __name__ == "__main__":
    unittest.main()
