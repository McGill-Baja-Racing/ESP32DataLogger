#!/usr/bin/env python3
"""Decode an ESP32 master binary SD log into CSV.

Usage:
    python3 tools/decode_log.py /path/to/log_0001.bin
    python3 tools/decode_log.py /path/to/log_0001.bin --output /path/to/log_0001.csv
"""

from __future__ import annotations

import argparse
import csv
import ctypes
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
NODE_DIAGNOSTICS_BASE = 0x0F0
NODE_SLOT_COUNT = 7
HEALTH_FAULT_COUNT = 17
# The 8-bit event subject holds the master-only GPS IDs 0x700-0x702 as 0x70-0x72.
HEALTH_SUBJECT_GPS_BASE = 0x70
HEALTH_EVENT_COUNT_MAX = 0x1FF
HEALTH_CODE_NAMES = [
    "sensor_init", "sensor_read", "sensor_dead", "samples_missed", "samples_dropped",
    "bad_timestamp", "no_time_sync", "can_tx", "can_rx_overflow", "can_no_ack",
    "can_bus_error", "can_passive", "can_bus_off", "sd_write", "gps_no_data",
    "gps_no_fix", "event_lost", "boot", "adc_uncalibrated", "node_offline",
]
HEALTH_ERROR_NAMES = [
    "ESP_OK", "ESP_FAIL", "ESP_ERR_NO_MEM", "ESP_ERR_INVALID_ARG", "ESP_ERR_INVALID_STATE",
    "ESP_ERR_NOT_FOUND", "ESP_ERR_TIMEOUT", "ESP_ERR_INVALID_RESPONSE", "other error",
]
HEALTH_NAMES = ["ok", "warning", "error", "critical"]
# Indexed by esp_reset_reason_t.
RESET_NAMES = [
    "unknown", "power_on", "external", "software", "panic", "interrupt_watchdog",
    "task_watchdog", "watchdog", "deep_sleep", "brownout", "sdio", "usb", "jtag",
    "efuse", "power_glitch", "cpu_lockup",
]
NODE_NAMES = [
    "master", "brake_node_1", "node_2", "mpu_node_3", "encoder_node_4", "engine_node_5",
    "adc_node_6",
]


# Payloads of node_status_frame_t, health_event_frame_t and node_diagnostics_frame_t in app_protocol.h.
class NodeStatus(ctypes.LittleEndianStructure):
    _fields_ = [
        ("faults", ctypes.c_uint32, 24),
        ("health", ctypes.c_uint32, 2),
        ("mode", ctypes.c_uint32, 1),
    ]


class HealthEvent(ctypes.LittleEndianStructure):
    _fields_ = [
        ("code", ctypes.c_uint32, 5),
        ("error", ctypes.c_uint32, 4),
        ("subject", ctypes.c_uint32, 8),
        ("sequence", ctypes.c_uint32, 6),
        ("count", ctypes.c_uint32, 9),
    ]


class NodeDiagnostics(ctypes.LittleEndianStructure):
    _fields_ = [
        ("tx_errors", ctypes.c_uint8),
        ("rx_errors", ctypes.c_uint8),
        ("resets", ctypes.c_uint8),
        ("reset_reason", ctypes.c_uint8),
    ]


def name_or_unknown(names: list[str], index: int) -> str:
    return names[index] if index < len(names) else "unknown"


def subject_can_id(subject: int) -> int:
    if HEALTH_SUBJECT_GPS_BASE <= subject <= HEALTH_SUBJECT_GPS_BASE + 2:
        return 0x700 + (subject - HEALTH_SUBJECT_GPS_BASE)
    return subject


def describe_health(can_id: int, packed: int) -> tuple[str, str, str] | None:
    """Return (signal, node, detail) for a status, event or diagnostics frame, else None."""
    payload = (packed & 0xFFFFFFFF).to_bytes(4, "little")
    if NODE_STATUS_BASE <= can_id < NODE_STATUS_BASE + NODE_SLOT_COUNT:
        node_id = can_id - NODE_STATUS_BASE
        status = NodeStatus.from_buffer_copy(payload)
        words = [HEALTH_NAMES[status.health], "recording" if status.mode else "idle"]
        words += [HEALTH_CODE_NAMES[f] for f in range(HEALTH_FAULT_COUNT) if status.faults >> f & 1]
        signal = "node_status"
    elif NODE_EVENT_BASE <= can_id < NODE_EVENT_BASE + NODE_SLOT_COUNT:
        node_id = can_id - NODE_EVENT_BASE
        event = HealthEvent.from_buffer_copy(payload)
        if event.code == HEALTH_CODE_NAMES.index("node_offline"):
            words = ["node_offline"] + [NODE_NAMES[n] for n in range(NODE_SLOT_COUNT) if event.subject >> n & 1]
        else:
            words = [
                name_or_unknown(HEALTH_CODE_NAMES, event.code),
                f"subject 0x{subject_can_id(event.subject):02X}",
                f"count {event.count}{'+' if event.count == HEALTH_EVENT_COUNT_MAX else ''}",
                name_or_unknown(HEALTH_ERROR_NAMES, event.error),
            ]
        signal = "health_event"
    elif NODE_DIAGNOSTICS_BASE <= can_id < NODE_DIAGNOSTICS_BASE + NODE_SLOT_COUNT:
        node_id = can_id - NODE_DIAGNOSTICS_BASE
        diagnostics = NodeDiagnostics.from_buffer_copy(payload)
        words = [
            f"tx_errors {diagnostics.tx_errors}", f"rx_errors {diagnostics.rx_errors}",
            f"resets {diagnostics.resets}",
            f"last_reset {name_or_unknown(RESET_NAMES, diagnostics.reset_reason)}",
        ]
        signal = "node_diagnostics"
    else:
        return None
    return signal, NODE_NAMES[node_id], " ".join(words)


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
