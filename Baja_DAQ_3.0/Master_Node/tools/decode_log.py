#!/usr/bin/env python3
"""Decode an ESP32 master binary SD log into CSV.

Usage:
    python3 tools/decode_log.py /path/to/log_0001.bin
    python3 tools/decode_log.py /path/to/log_0001.bin --output /path/to/log_0001.csv
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from pathlib import Path


RECORD_SIZE_BYTES = 16

SIGNAL_METADATA = {
    0x0B1: ("front_brake_pressure", "brake_node_1", "psi"),
    0x0B2: ("rear_brake_pressure", "brake_node_1", "psi"),
    0x0B9: ("bearing_encoder", "encoder_node_4", "deg_x10"),
    0x0BA: ("generic_adc_voltage", "engine_node_5", "mV"),
    0x0BB: ("engine_rpm", "engine_node_5", "rpm_placeholder"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode a master SD .bin log into CSV.")
    parser.add_argument("input", help="Path to the .bin log copied from the SD card.")
    parser.add_argument(
        "--output",
        help="CSV output path. Default: input filename with .csv suffix.",
    )
    return parser.parse_args()


def signed_u32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def decode_log(input_path: Path, output_path: Path) -> list[str]:
    data = input_path.read_bytes()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    full_records = len(data) // RECORD_SIZE_BYTES
    trailing_bytes = len(data) % RECORD_SIZE_BYTES
    counts_by_id: dict[int, int] = {}
    report: list[str] = []

    if trailing_bytes:
        report.append(
            f"Ignored {trailing_bytes} trailing byte(s); decoded complete 16-byte records only."
        )

    with output_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "sample_index",
                "can_id",
                "can_id_hex",
                "signal",
                "node",
                "timestamp_ms",
                "value",
                "units",
                "raw_data",
            ]
        )

        for index in range(full_records):
            offset = index * RECORD_SIZE_BYTES
            can_id_raw, packed_raw = struct.unpack_from("<qq", data, offset)
            can_id = int(can_id_raw) & 0x7FF
            packed = int(packed_raw) & 0xFFFFFFFFFFFFFFFF
            value = signed_u32(packed)
            timestamp_ms = (packed >> 32) & 0xFFFFFFFF
            signal, node, units = SIGNAL_METADATA.get(can_id, ("", "", "raw"))
            counts_by_id[can_id] = counts_by_id.get(can_id, 0) + 1

            writer.writerow(
                [
                    index,
                    can_id,
                    f"0x{can_id:03X}",
                    signal,
                    node,
                    timestamp_ms,
                    value,
                    units,
                    packed,
                ]
            )

    report.append(f"Decoded {full_records} complete sample record(s).")
    if counts_by_id:
        summary = ", ".join(
            f"0x{can_id:03X}={count}" for can_id, count in sorted(counts_by_id.items())
        )
        report.append(f"Found {len(counts_by_id)} CAN ID(s): {summary}.")
    return report


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    output_path = Path(args.output) if args.output else input_path.with_suffix(".csv")

    if not input_path.exists():
        print(f"Input file not found: {input_path}", file=sys.stderr)
        return 1

    try:
        report = decode_log(input_path, output_path)
    except OSError as exc:
        print(f"Decode failed: {exc}", file=sys.stderr)
        return 1

    print(f"Saved CSV: {output_path}")
    for line in report:
        print(f"  {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
