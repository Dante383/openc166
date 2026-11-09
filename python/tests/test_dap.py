# SPDX-License-Identifier: MIT
"""The DAP server, driven like an editor drives it: over pipes."""

import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

PROGRAM = """$MOD167
$SEGMENTED
CODE0   SECTION CODE AT 0
START:  DISWDT
        EINIT
        MOV     R4, #0
LOOP:   ADD     R4, #1
        MOV     0FC40H, R4
        CMP     R4, #10
        JMPR    cc_NE, LOOP
DONE:   JMPR    cc_UC, DONE
CODE0   ENDS
        END
"""


class Client:
    def __init__(self):
        env = dict(os.environ)
        self.p = subprocess.Popen(
            [sys.executable, "-m", "openc166.dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env
        )
        self.seq = 1
        self.messages = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            length = None
            while True:
                line = self.p.stdout.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            self.messages.put(json.loads(self.p.stdout.read(length)))

    def request(self, command, **arguments):
        seq = self.seq
        self.seq += 1
        body = json.dumps({"seq": seq, "type": "request", "command": command, "arguments": arguments}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()
        while True:
            m = self.messages.get(timeout=20)
            if m["type"] == "response" and m["request_seq"] == seq:
                return m
            self.pending.append(m)

    pending: list = []

    def wait_event(self, name):
        for i, m in enumerate(self.pending):
            if m.get("event") == name:
                return self.pending.pop(i)
        while True:
            m = self.messages.get(timeout=20)
            if m.get("event") == name:
                return m
            self.pending.append(m)

    def close(self):
        try:
            self.request("disconnect")
        finally:
            self.p.stdin.close()
            self.p.wait(timeout=10)
            self.p.stdout.close()


class DapTest(unittest.TestCase):
    def test_session(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "prog.a66")
            with open(path, "w") as f:
                f.write(PROGRAM)
            c = Client()
            c.pending = []
            try:
                r = c.request("initialize", adapterID="openc166")
                self.assertTrue(r["body"]["supportsStepBack"])
                self.assertTrue(c.request("launch", program=path, board="c167", stopOnEntry=False)["success"])
                c.wait_event("initialized")
                r = c.request("setBreakpoints", source={"path": path}, breakpoints=[{"line": 9, "condition": "r[4] == 3"}])
                self.assertTrue(r["body"]["breakpoints"][0]["verified"])
                c.request("configurationDone")
                ev = c.wait_event("stopped")
                self.assertEqual(ev["body"]["reason"], "breakpoint")

                frames = c.request("stackTrace", threadId=1)["body"]["stackFrames"]
                self.assertEqual(frames[0]["line"], 9)
                regs = c.request("variables", variablesReference=1)["body"]["variables"]
                self.assertEqual(next(v for v in regs if v["name"] == "R4")["value"], "0003H")

                c.request("next", threadId=1)
                c.wait_event("stopped")
                self.assertEqual(c.request("stackTrace", threadId=1)["body"]["stackFrames"][0]["line"], 10)
                c.request("stepBack", threadId=1)
                c.wait_event("stopped")
                self.assertEqual(c.request("stackTrace", threadId=1)["body"]["stackFrames"][0]["line"], 9)

                self.assertEqual(c.request("evaluate", expression="R4")["body"]["result"], "0003H")
                self.assertEqual(c.request("evaluate", expression="s.r[4] * 2")["body"]["result"], "6H (6)")

                pc = frames[0]["instructionPointerReference"]
                ins = c.request("disassemble", memoryReference=pc, instructionOffset=-2, instructionCount=5)
                texts = [i["instruction"] for i in ins["body"]["instructions"]]
                self.assertEqual(len(texts), 5)
                self.assertEqual(texts[2], "CMP     R4, #0AH")
                self.assertEqual(texts[1], "MOV     0FC40H, R4")

                mem = c.request("readMemory", memoryReference="0xFC40", count=2)
                self.assertEqual(mem["body"]["data"], "AwA=")  # 0003H

                info = c.request("dataBreakpointInfo", name="0xFC40")["body"]
                c.request("setDataBreakpoints", breakpoints=[{"dataId": info["dataId"], "accessType": "write"}])
                c.request("setBreakpoints", source={"path": path}, breakpoints=[])
                c.request("continue", threadId=1)
                ev = c.wait_event("stopped")
                self.assertEqual(ev["body"]["reason"], "data breakpoint")
                self.assertIn("0004H", ev["body"]["description"])
                c.request("reverseContinue", threadId=1)
                ev = c.wait_event("stopped")
                self.assertEqual(ev["body"]["reason"], "data breakpoint")
                self.assertIn("= 0003H", ev["body"]["description"])  # the write before
            finally:
                c.close()


if __name__ == "__main__":
    unittest.main()
