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
    0x0B3: ("mpu_accel_x", "mpu_node_3", "mg"),
    0x0B4: ("mpu_accel_y", "mpu_node_3", "mg"),
    0x0B5: ("mpu_accel_z", "mpu_node_3", "mg"),
    0x0B6: ("mpu_gyro_x", "mpu_node_3", "mdps"),
    0x0B7: ("mpu_gyro_y", "mpu_node_3", "mdps"),
    0x0B8: ("mpu_gyro_z", "mpu_node_3", "mdps"),
    0x0B9: ("bearing_rpm", "encoder_node_4", "rpm"),
    0x0BA: ("generic_adc_voltage", "adc_node_6", "mV"),
    0x0BB: ("engine_rpm", "engine_node_5", "rpm"),
    0x0BC: ("engine_spark", "engine_node_5", "event"),
    0x700: ("gps_speed", "master_gps", "km/h"),
    0x701: ("gps_latitude", "master_gps", "deg_e7"),
    0x702: ("gps_longitude", "master_gps", "deg_e7"),
}

# Health frames, matching health_code_t, health_error_t and node_health_t in app_protocol.h.
NODE_STATUS_BASE = 0x0D0
NODE_EVENT_BASE = 0x0E0
NODE_SLOT_COUNT = 7
HEALTH_FAULT_COUNT = 16
HEALTH_CODE_NAMES = [
    "sensor_init", "sensor_read", "sensor_dead", "samples_late", "samples_dropped",
    "bad_timestamp", "no_time_sync", "can_tx", "can_rx_overflow", "can_no_ack",
    "can_bus_error", "can_passive", "can_bus_off", "sd_write", "gps_no_data",
    "gps_no_fix", "boot", "adc_uncalibrated",
]
HEALTH_ERROR_NAMES = [
    "ESP_OK", "ESP_FAIL", "ESP_ERR_NO_MEM", "ESP_ERR_INVALID_ARG", "ESP_ERR_INVALID_STATE",
    "ESP_ERR_NOT_FOUND", "ESP_ERR_TIMEOUT", "ESP_ERR_INVALID_RESPONSE", "other error",
]
HEALTH_NAMES = ["ok", "warning", "error", "critical"]


def name_or_unknown(names: list[str], index: int) -> str:
    return names[index] if index < len(names) else "unknown"


def describe_health(can_id: int, packed: int) -> tuple[str, str, str] | None:
    """Return (signal, node, detail) for a status or event frame, else None."""
    if NODE_STATUS_BASE <= can_id < NODE_STATUS_BASE + NODE_SLOT_COUNT:
        node_id = can_id - NODE_STATUS_BASE
        faults = packed & 0xFFFF
        words = [
            name_or_unknown(HEALTH_NAMES, (packed >> 16) & 0xFF),
            "recording" if (packed >> 24) & 0xFF == 1 else "idle",
        ]
        words += [HEALTH_CODE_NAMES[f] for f in range(HEALTH_FAULT_COUNT) if faults >> f & 1]
        signal = "node_status"
    elif NODE_EVENT_BASE <= can_id < NODE_EVENT_BASE + NODE_SLOT_COUNT:
        node_id = can_id - NODE_EVENT_BASE
        code, subject, count, error = (packed >> (8 * i) & 0xFF for i in range(4))
        words = [
            name_or_unknown(HEALTH_CODE_NAMES, code),
            f"subject 0x{subject:02X}",
            f"count {count}",
            name_or_unknown(HEALTH_ERROR_NAMES, error),
        ]
        signal = "health_event"
    else:
        return None
    return signal, "master" if node_id == 0 else f"node_{node_id}", " ".join(words)


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
                "detail",
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
            detail = ""
            health = describe_health(can_id, packed)
            if health:
                signal, node, detail = health
                units = ""
            counts_by_id[can_id] = counts_by_id.get(can_id, 0) + 1

            writer.writerow(
                [
                    index,
                    can_id,
                    f"0x{can_id:03X}",
                    signal,
                    node,
                    timestamp_ms,
                    f"{value / 100:.2f}" if can_id == 0x700 else value,
                    units,
                    packed,
                    detail,
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
