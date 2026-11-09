# SPDX-License-Identifier: MIT
"""The serial bridge: a pyserial client talks to a simulated ASC0."""

import threading
import time
import unittest

import openc166
from openc166 import sim
from openc166.bridge import SerialBridge

try:
    import serial
except ImportError:  # pragma: no cover
    serial = None

# Answers every byte with the byte plus one, at 10416 baud (25 MHz).
FIRMWARE = """$MOD167
$SEGMENTED
$INCLUDE(REG167.INC)
CODE0   SECTION CODE AT 0
        DISWDT
        EINIT
        MOV     S0BG, #74
        MOV     S0CON, #8011H
LOOP:   JNB     S0RIR, LOOP
        BCLR    S0RIR
        MOV     R4, S0RBUF
        ADD     R4, #1
        MOV     S0TBUF, R4
        JMPR    cc_UC, LOOP
CODE0   ENDS
        END
"""


@unittest.skipIf(serial is None, "pyserial is not installed")
class BridgeTest(unittest.TestCase):
    def run_bridge(self, rfc2217):
        r = openc166.assemble(FIRMWARE)
        self.assertTrue(r.ok, r.errors)
        s = sim.Simulator.from_result(r, board="c167")
        bridge = SerialBridge(s, port=0, rfc2217=rfc2217, kline=False)
        bridge.listen()
        thread = threading.Thread(target=bridge.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(bridge.stop)
        return bridge

    def exchange(self, url):
        port = serial.serial_for_url(url, 10400, timeout=3)
        try:
            port.write(b"\x01\x41\xfe")
            got = b""
            deadline = time.time() + 5
            while len(got) < 3 and time.time() < deadline:
                got += port.read(3 - len(got))
            return got
        finally:
            port.close()

    def test_rfc2217(self):
        bridge = self.run_bridge(True)
        self.assertEqual(self.exchange(bridge.url), b"\x02\x42\xff")
        self.assertEqual(bridge.stats["from_ecu"], 3)

    def test_raw_socket(self):
        bridge = self.run_bridge(False)
        self.assertEqual(self.exchange(bridge.url), b"\x02\x42\xff")


if __name__ == "__main__":
    unittest.main()
