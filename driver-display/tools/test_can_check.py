"""Tests for can_check.py's decision to transmit, which need no CAN hardware.

Run: python3 -m unittest discover -s driver-display/tools -p 'test_*.py'
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import can_check  # noqa: E402

QUIET = {"frames": 0, "errors": 0, "fallback": False}


class TransmitDecision(unittest.TestCase):
    def test_never_transmits_unless_asked(self):
        run, reason = can_check.transmit_decision(want_tx=False, heard=QUIET, loop_ok=True)
        self.assertFalse(run)
        self.assertIn("--tx", reason)

    def test_transmits_on_a_quiet_bench_bus_when_asked(self):
        run, _ = can_check.transmit_decision(want_tx=True, heard=QUIET, loop_ok=True)
        self.assertTrue(run)

    def test_never_transmits_at_a_fallback_bitrate(self):
        # Listening at 500 kbit/s on a 1 Mbit/s bus can hear nothing, yet sending would disturb it.
        heard = dict(QUIET, fallback=True)
        run, reason = can_check.transmit_decision(want_tx=True, heard=heard, loop_ok=True)
        self.assertFalse(run)
        self.assertIn("bitrate", reason)

    def test_never_transmits_onto_a_busy_bus(self):
        for heard in (dict(QUIET, frames=3), dict(QUIET, errors=1)):
            run, _ = can_check.transmit_decision(want_tx=True, heard=heard, loop_ok=True)
            self.assertFalse(run)

    def test_needs_a_working_loopback_and_listen_step(self):
        self.assertFalse(can_check.transmit_decision(want_tx=True, heard=QUIET, loop_ok=False)[0])
        self.assertFalse(can_check.transmit_decision(want_tx=True, heard=None, loop_ok=True)[0])


if __name__ == "__main__":
    unittest.main()
