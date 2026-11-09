# SPDX-License-Identifier: MIT
"""The stock Siemens 5WY19 firmware on the simulator.

The dump is not part of openc166: set OPENC166_5WY19_IMAGE to its path
(e.g. ca663057_retail_virgin_immo_off.bin) to run these tests.
"""

import os
import unittest

from openc166 import sim

IMAGE = os.environ.get("OPENC166_5WY19_IMAGE")


@unittest.skipUnless(IMAGE and os.path.exists(IMAGE), "set OPENC166_5WY19_IMAGE to the stock dump")
class Stock5wy19Test(unittest.TestCase):
    def test_reset_self_tests_pass(self):
        # The startup code collects self-test failures in R15 and stores it
        # at 0E79EH (MOV 0E79EH, R15 at 0882D0H): register test, SYSCON
        # check, stack, internal RAM, XRAM and external RAM.
        s = sim.Simulator.from_file(IMAGE, board="5wy19")
        s.break_at(0x882D0)
        stop = s.cont(2_000_000)
        self.assertEqual(stop.reason, "breakpoint")
        self.assertEqual(s.r[15], 0x0000)
        self.assertEqual(s.sfr["SYSCON"], 0xE30E)
        self.assertTrue(s.einit_done)

    def test_boot_block_answers_kwp2000(self):
        # With the application signature at 9FFF0H erased, the boot block
        # runs its download loop and talks KWP2000 on the K-line.
        with open(IMAGE, "rb") as f:
            data = bytearray(f.read())
        data[0x1FFF0:0x1FFF4] = b"\xff" * 4
        s = sim.Simulator(bytes(data), board="5wy19")
        s.serial_echo = True
        s.set_adc(1, 0x2A0)
        s.run_for(0.1)
        s.serial_receive()

        def request(pdu, wait=0.4):
            frame = bytes([0x80 + len(pdu), 0x11, 0xF1]) + pdu
            frame += bytes([sum(frame) & 0xFF])
            s.serial_send(frame)
            s.run_for(wait)
            reply = s.serial_receive(baud=10400)  # what a tester at 10400 baud hears
            self.assertEqual(sum(reply[:-1]) & 0xFF, reply[-1], "checksum")
            return reply[3:-1] if reply[0] != 0x80 else reply[4:-1]

        s.run_for(0.025)
        s.serial_break(0.025)
        s.run_for(0.025)
        self.assertEqual(request(b"\x81")[0], 0xC1)
        self.assertEqual(request(b"\x10\x85"), b"\x50\x85")
        ident = request(b"\x1a\x80")
        self.assertEqual(ident[:2], b"\x5a\x80")
        self.assertIn(b"G7DCMQ1A", ident)
        self.assertEqual(request(b"\x3e\x01")[0], 0x7E)

        # SecurityAccess is held off for about 10 s after power-on. The
        # Siemens level (0FDH) compares with a key stored in flash; at that
        # level ReadMemoryByAddress may read the flash.
        for _ in range(40):
            seed = request(b"\x27\xfd")
            if seed[:2] != b"\x7f\x27":
                break
            request(b"\x3e\x01")
        self.assertEqual(seed[:2], b"\x67\xfd")
        key = bytes(data[0x3F60:0x3F64])
        self.assertEqual(request(b"\x27\xfe" + key), b"\x67\xfe\x34")
        self.assertEqual(request(b"\x23\x09\x00\x40\x04"), b"\x63ca66")

    def test_application_answers_kwp2000(self):
        # The application's scheduler runs from CAPCOM (CC26 on T8); with it
        # the application services the watchdog and answers on the K-line.
        with open(IMAGE, "rb") as f:
            data = f.read()
        s = sim.Simulator(data, board="5wy19")
        s.serial_echo = True
        s.set_adc(1, 0x2A0)
        s.run_for(0.6)
        self.assertTrue(s.capcom_output(26) in (True, False))
        s.serial_receive()

        def request(pdu):
            frame = bytes([0x80 + len(pdu), 0x11, 0xF1]) + pdu
            frame += bytes([sum(frame) & 0xFF])
            s.serial_send(frame)
            s.run_for(0.4)
            reply = s.serial_receive(baud=10400)
            self.assertTrue(reply, "no answer to " + pdu.hex())
            return reply[3:-1] if reply[0] != 0x80 else reply[4:-1]

        s.run_for(0.025)
        s.serial_break(0.025)
        s.run_for(0.025)
        self.assertEqual(request(b"\x81")[0], 0xC1)
        self.assertEqual(request(b"\x10\x85"), b"\x50\x85")
        ident = request(b"\x1a\x80")
        self.assertIn(b"G7DCMQ1A", ident)
        self.assertEqual(s.sfr["WDTCON"] & 0x02, 0, "the watchdog reset the CPU")

    def test_disassembly_round_trip(self):
        with open(IMAGE, "rb") as f:
            data = f.read()
        text, fallbacks = sim.disassemble_module(data[0x8000:0x10000], 0x88000)
        self.assertEqual(fallbacks, 0)
        import openc166

        r = openc166.assemble(text)
        self.assertTrue(r.ok)
        self.assertEqual(r.image(base=0x88000).data, data[0x8000:0x10000])


if __name__ == "__main__":
    unittest.main()
