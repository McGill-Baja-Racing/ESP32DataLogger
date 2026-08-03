#!/usr/bin/env python3
"""Check a DAQ 3.0 binary or decoded CSV log for missing sensor messages."""

from __future__ import annotations

import argparse
import csv
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


RECORD_SIZE_BYTES = 16
TIMESTAMP_MODULUS = 1 << 32
WRAP_HIGH_WATERMARK = 0xF0000000
WRAP_LOW_WATERMARK = 0x0FFFFFFF


@dataclass(frozen=True)
class Signal:
    can_id: int
    name: str
    node_id: int
    units: str
    rate_hz: int

    @property
    def period_ms(self) -> int:
        return 1000 // self.rate_hz


SIGNALS = (
    Signal(0x0B1, "front_brake_pressure", 1, "psi", 100),
    Signal(0x0B2, "rear_brake_pressure", 1, "psi", 100),
    Signal(0x0B9, "bearing_rpm", 4, "rpm", 50),
    Signal(0x0BA, "generic_adc_voltage", 6, "mV", 100),
    Signal(0x0BB, "engine_rpm", 5, "rpm", 50),
)
SIGNALS_BY_ID = {signal.can_id: signal for signal in SIGNALS}
KNOWN_NODE_IDS = {signal.node_id for signal in SIGNALS}


@dataclass(frozen=True)
class Record:
    index: int
    can_id: int
    timestamp_ms: int


@dataclass(frozen=True)
class Anomaly:
    can_id: int
    signal: str
    previous_index: int
    current_index: int
    previous_timestamp_ms: int
    current_timestamp_ms: int
    actual_interval_ms: int | None
    expected_interval_ms: int
    estimated_missing: int
    anomaly_type: str


@dataclass
class SignalStats:
    signal: Signal
    observed: int = 0
    missing: int = 0
    normal_intervals: int = 0
    duplicate_timestamps: int = 0
    gaps: int = 0
    timestamp_resets: int = 0
    intervals: list[int] = field(default_factory=list)
    largest_gap_ms: int = 0

    @property
    def loss_percent(self) -> float | None:
        total = self.observed + self.missing
        return self.missing * 100.0 / total if total else None


@dataclass
class Analysis:
    stats: dict[int, SignalStats]
    anomalies: list[Anomaly]
    unknown_counts: dict[int, int]
    trailing_bytes: int
    total_records: int
    absent_expected_ids: set[int]
    diagnostic_counts: dict[int, int] = field(default_factory=dict)

    @property
    def has_warnings(self) -> bool:
        return (
            self.total_records == 0
            or self.trailing_bytes > 0
            or bool(self.unknown_counts)
            or bool(self.absent_expected_ids)
            or bool(self.anomalies)
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check a DAQ 3.0 binary or CSV log for missing sensor messages."
    )
    parser.add_argument("input", help="Master .bin log or decoded .csv log to check")
    parser.add_argument(
        "--expected-nodes",
        nargs="+",
        type=int,
        metavar="NODE",
        help="Override MASTER_EXPECTED_NODE_MASK for this run",
    )
    parser.add_argument(
        "--gaps-output",
        help="Detailed anomaly CSV path (default: <input_stem>_gaps.csv)",
    )
    return parser.parse_args()


def configured_node_ids(header_path: Path) -> set[int]:
    """Read node IDs from the shifts in MASTER_EXPECTED_NODE_MASK."""
    text = header_path.read_text(encoding="utf-8")
    match = re.search(
        r"#define\s+MASTER_EXPECTED_NODE_MASK\s+([^\n]*(?:\\\n[^\n]*)*)",
        text,
    )
    if not match:
        raise ValueError(f"MASTER_EXPECTED_NODE_MASK not found in {header_path}")
    expression = re.sub(r"/\*.*?\*/|//.*", "", match.group(1), flags=re.DOTALL)
    nodes = {int(value) for value in re.findall(r"1U?\s*<<\s*(\d+)", expression)}
    if not nodes:
        raise ValueError(
            "MASTER_EXPECTED_NODE_MASK must contain entries like (1U << 4), "
            "or use --expected-nodes"
        )
    unknown = nodes - KNOWN_NODE_IDS
    if unknown:
        raise ValueError(f"Configured node(s) have no known sensor mapping: {sorted(unknown)}")
    return nodes


def expected_signal_ids(node_ids: Iterable[int]) -> set[int]:
    node_set = set(node_ids)
    unknown = node_set - KNOWN_NODE_IDS
    if unknown:
        raise ValueError(f"Unknown expected node ID(s): {sorted(unknown)}")
    return {signal.can_id for signal in SIGNALS if signal.node_id in node_set}


def read_binary_records(path: Path) -> tuple[list[Record], int]:
    data = path.read_bytes()
    full_records = len(data) // RECORD_SIZE_BYTES
    records: list[Record] = []
    for index in range(full_records):
        can_id_raw, packed_raw = struct.unpack_from("<qq", data, index * RECORD_SIZE_BYTES)
        can_id = int(can_id_raw) & 0x7FF
        packed = int(packed_raw) & 0xFFFFFFFFFFFFFFFF
        records.append(Record(index, can_id, (packed >> 32) & 0xFFFFFFFF))
    return records, len(data) % RECORD_SIZE_BYTES


def parse_csv_can_id(row: dict[str, str], row_number: int) -> int:
    value = (row.get("can_id") or row.get("can_id_hex") or "").strip()
    if not value:
        raise ValueError(f"CSV row {row_number} has no CAN ID")
    try:
        return int(value, 0) & 0x7FF
    except ValueError as error:
        raise ValueError(f"CSV row {row_number} has invalid CAN ID {value!r}") from error


def read_csv_records(path: Path) -> tuple[list[Record], int]:
    records: list[Record] = []
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        fields = set(reader.fieldnames or ())
        if "timestamp_ms" not in fields or not ({"can_id", "can_id_hex"} & fields):
            raise ValueError(
                "CSV must contain timestamp_ms and either can_id or can_id_hex columns"
            )
        for row_number, row in enumerate(reader, start=2):
            timestamp_text = (row.get("timestamp_ms") or "").strip()
            try:
                timestamp = int(timestamp_text, 0)
            except ValueError as error:
                raise ValueError(
                    f"CSV row {row_number} has invalid timestamp_ms {timestamp_text!r}"
                ) from error
            if not 0 <= timestamp < TIMESTAMP_MODULUS:
                raise ValueError(
                    f"CSV row {row_number} timestamp_ms is outside uint32 range"
                )
            records.append(
                Record(len(records), parse_csv_can_id(row, row_number), timestamp)
            )
    return records, 0


def read_records(path: Path) -> tuple[list[Record], int]:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        return read_csv_records(path)
    if suffix == ".bin":
        return read_binary_records(path)
    raise ValueError("Input must have a .bin or .csv extension")


def timestamp_delta(previous: int, current: int) -> tuple[int | None, bool]:
    if current >= previous:
        return current - previous, False
    if previous >= WRAP_HIGH_WATERMARK and current <= WRAP_LOW_WATERMARK:
        return TIMESTAMP_MODULUS - previous + current, False
    return None, True


def analyze_records(
    records: Iterable[Record], expected_ids: set[int], trailing_bytes: int = 0
) -> Analysis:
    record_list = list(records)
    present_known_ids = {record.can_id for record in record_list if record.can_id in SIGNALS_BY_ID}
    tracked_ids = present_known_ids | expected_ids
    stats = {can_id: SignalStats(SIGNALS_BY_ID[can_id]) for can_id in sorted(tracked_ids)}
    unknown_counts: dict[int, int] = {}
    diagnostic_counts: dict[int, int] = {}
    anomalies: list[Anomaly] = []
    previous_by_id: dict[int, Record] = {}

    for record in record_list:
        signal = SIGNALS_BY_ID.get(record.can_id)
        if signal is None:
            if 0x0D1 <= record.can_id <= 0x0D6:
                diagnostic_counts[record.can_id] = diagnostic_counts.get(record.can_id, 0) + 1
                continue
            unknown_counts[record.can_id] = unknown_counts.get(record.can_id, 0) + 1
            continue
        current_stats = stats[record.can_id]
        current_stats.observed += 1
        previous = previous_by_id.get(record.can_id)
        previous_by_id[record.can_id] = record
        if previous is None:
            continue

        delta, reset = timestamp_delta(previous.timestamp_ms, record.timestamp_ms)
        if reset:
            current_stats.timestamp_resets += 1
            anomalies.append(
                Anomaly(
                    record.can_id, signal.name, previous.index, record.index,
                    previous.timestamp_ms, record.timestamp_ms, None,
                    signal.period_ms, 0, "timestamp_reset",
                )
            )
            continue
        assert delta is not None
        if delta == 0:
            current_stats.duplicate_timestamps += 1
            anomalies.append(
                Anomaly(
                    record.can_id, signal.name, previous.index, record.index,
                    previous.timestamp_ms, record.timestamp_ms, 0,
                    signal.period_ms, 0, "duplicate_timestamp",
                )
            )
            continue

        current_stats.intervals.append(delta)
        if delta * 2 >= signal.period_ms * 3:
            estimated_missing = max(round(delta / signal.period_ms) - 1, 1)
            current_stats.missing += estimated_missing
            current_stats.gaps += 1
            current_stats.largest_gap_ms = max(current_stats.largest_gap_ms, delta)
            anomalies.append(
                Anomaly(
                    record.can_id, signal.name, previous.index, record.index,
                    previous.timestamp_ms, record.timestamp_ms, delta,
                    signal.period_ms, estimated_missing, "gap",
                )
            )
        else:
            current_stats.normal_intervals += 1

    return Analysis(
        stats=stats,
        anomalies=anomalies,
        unknown_counts=unknown_counts,
        trailing_bytes=trailing_bytes,
        total_records=len(record_list),
        absent_expected_ids=expected_ids - present_known_ids,
        diagnostic_counts=diagnostic_counts,
    )


def interval_text(values: list[int], operation: str) -> str:
    if not values:
        return "n/a"
    if operation == "min":
        return str(min(values))
    if operation == "max":
        return str(max(values))
    return f"{sum(values) / len(values):.2f}"


def print_report(analysis: Analysis) -> None:
    result = "WARNING" if analysis.has_warnings else "PASS"
    print(f"{result}: checked {analysis.total_records} complete record(s).")
    if analysis.total_records == 0:
        print("  File contains no complete records.")
    if analysis.trailing_bytes:
        print(f"  Incomplete trailing data: {analysis.trailing_bytes} byte(s).")
    if analysis.unknown_counts:
        values = ", ".join(
            f"0x{can_id:03X}={count}" for can_id, count in sorted(analysis.unknown_counts.items())
        )
        print(f"  Unknown CAN IDs: {values}")
    if analysis.diagnostic_counts:
        values=", ".join(f"node {can_id-0x0D0}={count}" for can_id,count in sorted(analysis.diagnostic_counts.items()))
        print(f"  Diagnostic records: {values}")
    if analysis.absent_expected_ids:
        names = ", ".join(
            f"0x{can_id:03X} {SIGNALS_BY_ID[can_id].name}"
            for can_id in sorted(analysis.absent_expected_ids)
        )
        print(f"  Configured signals entirely absent: {names}")

    headers = (
        "CAN ID", "Signal", "Seen", "Missing", "Loss %", "Normal", "Duplicate",
        "Gaps", "Resets", "Interval ms min/avg/max", "Expected", "Worst gap",
    )
    rows: list[tuple[str, ...]] = []
    for can_id, item in sorted(analysis.stats.items()):
        loss = "n/a" if item.loss_percent is None else f"{item.loss_percent:.3f}"
        interval_summary = "/".join(
            (interval_text(item.intervals, "min"), interval_text(item.intervals, "avg"),
             interval_text(item.intervals, "max"))
        )
        rows.append((
            f"0x{can_id:03X}", item.signal.name, str(item.observed), str(item.missing),
            loss, str(item.normal_intervals), str(item.duplicate_timestamps),
            str(item.gaps), str(item.timestamp_resets), interval_summary,
            str(item.signal.period_ms), str(item.largest_gap_ms or "n/a"),
        ))
    widths = [len(value) for value in headers]
    for row in rows:
        widths = [max(width, len(value)) for width, value in zip(widths, row)]
    print()
    print("  ".join(value.ljust(width) for value, width in zip(headers, widths)))
    print("  ".join("-" * width for width in widths))
    for row in rows:
        print("  ".join(value.ljust(width) for value, width in zip(row, widths)))


def write_anomalies(path: Path, anomalies: Iterable[Anomaly]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow((
            "can_id", "can_id_hex", "signal", "previous_record_index",
            "current_record_index", "previous_timestamp_ms", "current_timestamp_ms",
            "actual_interval_ms", "expected_interval_ms", "estimated_missing",
            "anomaly_type",
        ))
        for item in anomalies:
            writer.writerow((
                item.can_id, f"0x{item.can_id:03X}", item.signal,
                item.previous_index, item.current_index, item.previous_timestamp_ms,
                item.current_timestamp_ms,
                "" if item.actual_interval_ms is None else item.actual_interval_ms,
                item.expected_interval_ms, item.estimated_missing, item.anomaly_type,
            ))


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    gaps_path = (
        Path(args.gaps_output)
        if args.gaps_output
        else input_path.with_name(f"{input_path.stem}_gaps.csv")
    )
    try:
        if args.expected_nodes is not None:
            node_ids = set(args.expected_nodes)
        else:
            project_root = Path(__file__).resolve().parents[1]
            node_ids = configured_node_ids(project_root / "src/node_state/node_registry.h")
        expected_ids = expected_signal_ids(node_ids)
        records, trailing_bytes = read_records(input_path)
        analysis = analyze_records(records, expected_ids, trailing_bytes)
        write_anomalies(gaps_path, analysis.anomalies)
    except (OSError, ValueError) as error:
        print(f"Log check failed: {error}", file=sys.stderr)
        return 2

    print_report(analysis)
    print(f"\nDetailed anomalies: {gaps_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
