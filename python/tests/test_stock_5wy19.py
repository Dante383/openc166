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
