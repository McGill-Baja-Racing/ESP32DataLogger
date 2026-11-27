"""Tests for daq_sim.py that need no CAN interface.

Run: python3 -m unittest discover -s driver-display/tools -p 'test_*.py'
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import daq_sim  # noqa: E402


def payload(value, timestamp_ms):
    return (timestamp_ms << 32) | (value & 0xFFFFFFFF)


class RecordToFrames(unittest.TestCase):
    def test_brake_sample_is_sent_as_logged(self):
        # The front brake record of docs/LOG_FORMAT.md: 1500 psi at 1000 ms.
        self.assertEqual(daq_sim.record_to_frames(0x0B1, payload(1500, 1000), 0),
                         [(0x0B1, bytes.fromhex('DC050000E8030000'))])

    def test_negative_sample_keeps_its_bits(self):
        self.assertEqual(daq_sim.record_to_frames(0x0B9, payload(-1, 5), 0),
                         [(0x0B9, bytes.fromhex('FFFFFFFF05000000'))])

    def test_status_goes_out_at_its_wire_length(self):
        # The master appended its arrival time; nodes send only the 4-byte payload.
        frames = daq_sim.record_to_frames(0x0D4, payload(0x12345678, 999), 0)
        self.assertEqual(frames, [(0x0D4, bytes.fromhex('78563412'))])

    def test_diagnostics_goes_out_at_its_wire_length(self):
        frames = daq_sim.record_to_frames(0x0F8, payload(0x01020304, 999), 0)
        self.assertEqual(frames, [(0x0F8, bytes.fromhex('04030201'))])

    def test_event_is_sent_whole(self):
        frames = daq_sim.record_to_frames(0x0E3, payload(0x0A0B0C0D, 77), 0)
        self.assertEqual(frames, [(0x0E3, bytes.fromhex('0D0C0B0A4D000000'))])

    def test_aim_record_becomes_the_aim_wire_frame(self):
        self.assertEqual(daq_sim.record_to_frames(0x703, payload(2000, 50), 5),
                         [(0x500, bytes([0xD0, 0x07, 0x05, 0x00]))])

    def test_master_only_records_are_never_sent(self):
        for log_id in (0x0D0, 0x0E0, 0x0F0, 0x700, 0x701, 0x702, 0x123):
            self.assertEqual(daq_sim.record_to_frames(log_id, payload(1, 1), 0), [], hex(log_id))


class ReadRecords(unittest.TestCase):
    def test_ignores_an_incomplete_trailing_record(self):
        record = (0x0B1).to_bytes(8, 'little') + payload(1500, 1000).to_bytes(8, 'little')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.bin'
            path.write_bytes(record + record + b'\x00' * 8)
            self.assertEqual(daq_sim.read_records(path),
                             [(0x0B1, payload(1500, 1000)), (0x0B1, payload(1500, 1000))])


class Guard(unittest.TestCase):
    def test_refuses_a_real_interface(self):
        result = subprocess.run([sys.executable, str(TOOLS / 'daq_sim.py'), 'can0'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('refusing to transmit on can0', result.stderr)


class Synthetic(unittest.TestCase):
    def test_fake_values_stay_plausible(self):
        sim = daq_sim.Synthetic(nodes={1, 3, 4, 5, 7, 8})
        for t_ms in range(0, 70000, 37):
            for can_id, data in sim.samples_due(t_ms):
                value = int.from_bytes(data[:4], 'little', signed=True)
                if can_id in (0x0B1, 0x0B2):
                    self.assertTrue(0 <= value <= 1500, (hex(can_id), value))
                if can_id == 0x0BB:
                    self.assertTrue(value == 0 or 1500 <= value <= 3800, value)
                if can_id in (0x0BD, 0x0BE, 0x0BF, 0x0C9):
                    self.assertTrue(0 <= value <= 150, value)
                self.assertEqual(int.from_bytes(data[4:], 'little'), t_ms)


if __name__ == '__main__':
    unittest.main()
