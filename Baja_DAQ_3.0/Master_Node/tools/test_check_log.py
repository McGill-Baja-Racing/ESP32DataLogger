#!/usr/bin/env python3

import struct
import tempfile
import unittest
import csv
from pathlib import Path

import check_log


def records(can_id: int, timestamps: list[int], start_index: int = 0) -> list[check_log.Record]:
    return [
        check_log.Record(start_index + index, can_id, timestamp)
        for index, timestamp in enumerate(timestamps)
    ]


class AnalyzeLogTests(unittest.TestCase):
    def test_clean_100_hz_stream(self) -> None:
        result = check_log.analyze_records(records(0x0B1, [100, 110, 120, 130]), {0x0B1})
        stats = result.stats[0x0B1]
        self.assertFalse(result.has_warnings)
        self.assertEqual(stats.normal_intervals, 3)
        self.assertEqual(stats.missing, 0)

    def test_clean_50_hz_stream(self) -> None:
        result = check_log.analyze_records(records(0x0B9, [100, 120, 140]), {0x0B9})
        self.assertFalse(result.has_warnings)
        self.assertEqual(result.stats[0x0B9].normal_intervals, 2)

    def test_single_and_consecutive_missing_messages(self) -> None:
        result = check_log.analyze_records(
            records(0x0B1, [100, 120, 160]), {0x0B1}
        )
        stats = result.stats[0x0B1]
        self.assertEqual(stats.gaps, 2)
        self.assertEqual(stats.missing, 4)
        self.assertAlmostEqual(stats.loss_percent, 4 / 7 * 100)
        self.assertEqual([item.estimated_missing for item in result.anomalies], [1, 3])

    def test_interleaved_ids_are_checked_independently(self) -> None:
        stream = [
            check_log.Record(0, 0x0B1, 100),
            check_log.Record(1, 0x0B9, 100),
            check_log.Record(2, 0x0B1, 110),
            check_log.Record(3, 0x0B9, 120),
        ]
        result = check_log.analyze_records(stream, {0x0B1, 0x0B9})
        self.assertFalse(result.has_warnings)

    def test_absent_configured_signal_is_reported_without_inventing_loss(self) -> None:
        result = check_log.analyze_records(records(0x0B9, [100, 120]), {0x0B9, 0x0BB})
        self.assertEqual(result.absent_expected_ids, {0x0BB})
        self.assertEqual(result.stats[0x0BB].observed, 0)
        self.assertEqual(result.stats[0x0BB].missing, 0)
        self.assertIsNone(result.stats[0x0BB].loss_percent)

    def test_duplicate_and_reset_are_separate_anomalies(self) -> None:
        result = check_log.analyze_records(
            records(0x0B9, [100, 100, 90]), {0x0B9}
        )
        stats = result.stats[0x0B9]
        self.assertEqual(stats.duplicate_timestamps, 1)
        self.assertEqual(stats.timestamp_resets, 1)
        self.assertEqual([item.anomaly_type for item in result.anomalies],
                         ["duplicate_timestamp", "timestamp_reset"])

    def test_timestamp_wrap_is_a_normal_forward_interval(self) -> None:
        previous = 0xFFFFFFF5
        current = 9
        result = check_log.analyze_records(records(0x0B9, [previous, current]), {0x0B9})
        stats = result.stats[0x0B9]
        self.assertEqual(stats.normal_intervals, 1)
        self.assertEqual(stats.intervals, [20])
        self.assertEqual(stats.timestamp_resets, 0)

    def test_unknown_id_and_trailing_bytes_warn(self) -> None:
        result = check_log.analyze_records(
            [check_log.Record(0, 0x321, 100)], set(), trailing_bytes=3
        )
        self.assertTrue(result.has_warnings)
        self.assertEqual(result.unknown_counts, {0x321: 1})
        self.assertEqual(result.trailing_bytes, 3)

    def test_diagnostic_ids_are_summarized_not_treated_as_samples(self) -> None:
        result = check_log.analyze_records(
            [check_log.Record(0, 0x0D5, 100)], set()
        )
        self.assertFalse(result.has_warnings)
        self.assertEqual(result.diagnostic_counts, {0x0D5: 1})
        self.assertEqual(result.unknown_counts, {})
        self.assertNotIn(0x0D5, result.stats)

    def test_empty_log_warns(self) -> None:
        result = check_log.analyze_records([], set())
        self.assertTrue(result.has_warnings)
        self.assertEqual(result.total_records, 0)

    def test_read_records_ignores_trailing_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.bin"
            packed = (100 << 32) | 42
            path.write_bytes(struct.pack("<qq", 0x0B9, packed) + b"abc")
            parsed, trailing = check_log.read_binary_records(path)
        self.assertEqual(trailing, 3)
        self.assertEqual(parsed, [check_log.Record(0, 0x0B9, 100)])

    def test_reads_configured_node_mask(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "node_registry.h"
            path.write_text(
                "#define MASTER_EXPECTED_NODE_MASK ((1U << 4) | (1U << 5))\n",
                encoding="utf-8",
            )
            self.assertEqual(check_log.configured_node_ids(path), {4, 5})

    def test_reads_decoder_csv(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.csv"
            path.write_text(
                "sample_index,can_id,can_id_hex,timestamp_ms,value\n"
                "0,185,0x0B9,100,500\n"
                "1,185,0x0B9,120,500\n",
                encoding="utf-8",
            )
            parsed, trailing = check_log.read_records(path)
        self.assertEqual(trailing, 0)
        self.assertEqual(
            parsed,
            [check_log.Record(0, 0x0B9, 100), check_log.Record(1, 0x0B9, 120)],
        )

    def test_reads_csv_with_hex_id_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.csv"
            path.write_text(
                "can_id_hex,timestamp_ms\n0x0BB,200\n",
                encoding="utf-8",
            )
            parsed, _ = check_log.read_records(path)
        self.assertEqual(parsed, [check_log.Record(0, 0x0BB, 200)])

    def test_rejects_csv_without_required_columns(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.csv"
            path.write_text("time,value\n100,5\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "timestamp_ms"):
                check_log.read_records(path)

    def test_writes_detailed_anomaly_csv(self) -> None:
        result = check_log.analyze_records(records(0x0B1, [100, 120]), {0x0B1})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gaps.csv"
            check_log.write_anomalies(path, result.anomalies)
            with path.open(newline="", encoding="utf-8") as source:
                rows = list(csv.DictReader(source))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["anomaly_type"], "gap")
        self.assertEqual(rows[0]["estimated_missing"], "1")


if __name__ == "__main__":
    unittest.main()
