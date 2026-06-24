#!/usr/bin/env python3
"""Download a Base64-encoded log file from the ESP32 master over serial.

macOS setup:
    1. Close PlatformIO monitor before running this script. Only one program can
       use the serial port at a time.
    2. From the Master project folder, install dependencies into the local venv:
           .venv/bin/python -m pip install pyserial
    3. Find the serial port:
           ls /dev/cu.usb*

Usage from the Master project folder:
    .venv/bin/python tools/download_log.py --port /dev/cu.usbmodem5B140754501 --file log_0003.bin

Optional output path:
    .venv/bin/python tools/download_log.py --port /dev/cu.usbmodem5B140754501 --file log_0003.bin --output downloads/log_0003.bin

The firmware command must already exist on the master:
    download <filename>

The master responds with:
    BEGIN_FILE name=<filename> size=<bytes> encoding=base64
    <base64 chunks>
    END_FILE name=<filename> bytes=<bytes>
"""

from __future__ import annotations

import argparse
import base64
import csv
import re
import struct
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "Missing dependency: pyserial\n"
        "Install it with: python -m pip install pyserial"
    ) from exc


BEGIN_RE = re.compile(
    r"BEGIN_FILE\s+name=(?P<name>\S+)\s+size=(?P<size>\d+)\s+encoding=base64"
)
END_RE = re.compile(r"END_FILE\s+name=(?P<name>\S+)\s+bytes=(?P<bytes>\d+)")
ERROR_RE = re.compile(r"ERROR_FILE\s+name=(?P<name>\S+)\s+reason=(?P<reason>\S+)")
BASE64_LINE_RE = re.compile(r"^[A-Za-z0-9+/]*={0,2}$")
RECORD_SIZE_BYTES = 16

SIGNAL_METADATA = {
    0x0B1: ("front_brake_pressure", "brake_node_1", "psi_x10"),
    0x0B2: ("rear_brake_pressure", "brake_node_1", "psi_x10"),
    0x0B9: ("bearing_encoder", "encoder_node_4", "deg_x10"),
    0x700: ("gps_speed", "master", "km/h"),
    0x701: ("gps_latitude", "master", "deg_e7"),
    0x702: ("gps_longitude", "master", "deg_e7"),
    0x703: ("engine_rpm", "master", "rpm"),
}


def print_progress(received_bytes: int, total_bytes: int, *, done: bool = False) -> None:
    width = 32
    if total_bytes <= 0:
        print(f"\rReceiving: {received_bytes} bytes", end="", flush=True)
        if done:
            print()
        return

    received_bytes = min(received_bytes, total_bytes)
    filled = int(width * received_bytes / total_bytes)
    bar = "#" * filled + "-" * (width - filled)
    pct = 100.0 * received_bytes / total_bytes
    print(
        f"\rReceiving: [{bar}] {pct:6.2f}% "
        f"({received_bytes}/{total_bytes} bytes)",
        end="",
        flush=True,
    )
    if done:
        print()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download a log file from the ESP32 master SD card over serial."
    )
    parser.add_argument(
        "--port",
        required=True,
        help="Serial port, for example /dev/cu.usbmodem5B140754501",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate. Default: 115200",
    )
    parser.add_argument(
        "--file",
        required=True,
        help="Filename on the SD card, for example log_0003.bin",
    )
    parser.add_argument(
        "--output",
        help="Local output path. Default: downloads/<filename>",
    )
    parser.add_argument(
        "--csv-output",
        help="Local CSV output path. Default: same as output with .csv suffix",
    )
    parser.add_argument(
        "--no-csv",
        action="store_true",
        help="Only save the downloaded .bin file; do not create a CSV.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="Seconds to wait for serial data before failing. Default: 20",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=2.0,
        help="Seconds to wait after opening the port before sending the command. Default: 2",
    )
    return parser.parse_args()


def clean_line(raw: bytes) -> str:
    return raw.decode("utf-8", errors="replace").strip()


def is_base64_payload_line(line: str) -> bool:
    if not line:
        return False
    if len(line) % 4 != 0:
        return False
    return BASE64_LINE_RE.fullmatch(line) is not None


def csv_path_for_bin(bin_path: Path, csv_output: str | None) -> Path:
    if csv_output:
        return Path(csv_output)
    return bin_path.with_suffix(".csv")


def signed_u32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def display_value(can_id: int, value: int) -> int | float:
    if can_id == 0x700:
        return value / 100.0
    return value


def convert_bin_to_csv(bin_path: Path, csv_path: Path) -> list[str]:
    data = bin_path.read_bytes()
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    full_records = len(data) // RECORD_SIZE_BYTES
    trailing_bytes = len(data) % RECORD_SIZE_BYTES
    report: list[str] = []
    if trailing_bytes:
        report.append(
            f"Ignored {trailing_bytes} trailing byte(s); decoded complete 16-byte records only."
        )

    counts_by_id: dict[int, int] = {}
    with csv_path.open("w", newline="", encoding="utf-8") as output:
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
                    display_value(can_id, value),
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


def download_file(args: argparse.Namespace) -> Path:
    output = Path(args.output) if args.output else Path("downloads") / Path(args.file).name
    output.parent.mkdir(parents=True, exist_ok=True)

    b64_lines: list[str] = []
    expected_size: int | None = None
    reported_bytes: int | None = None
    received_encoded_bytes = 0
    in_file = False
    last_data_at = time.monotonic()

    with serial.Serial(args.port, args.baud, timeout=0.25) as ser:
        time.sleep(args.settle)
        ser.reset_input_buffer()

        command = f"download {args.file}\n".encode("utf-8")
        ser.write(command)
        ser.flush()
        print(f"Sent command: download {args.file}")

        while True:
            if time.monotonic() - last_data_at > args.timeout:
                raise TimeoutError("Timed out waiting for file transfer data")

            raw = ser.readline()
            if not raw:
                continue

            last_data_at = time.monotonic()
            line = clean_line(raw)
            if not line:
                continue

            error_match = ERROR_RE.search(line)
            if error_match:
                raise RuntimeError(
                    f"Device reported transfer error for {error_match.group('name')}: "
                    f"{error_match.group('reason')}"
                )

            if not in_file:
                begin_match = BEGIN_RE.search(line)
                if not begin_match:
                    print(line)
                    continue

                expected_size = int(begin_match.group("size"))
                in_file = True
                print(
                    f"Receiving {begin_match.group('name')} "
                    f"({expected_size} bytes, Base64)..."
                )
                print_progress(0, expected_size)
                continue

            end_match = END_RE.search(line)
            if end_match:
                reported_bytes = int(end_match.group("bytes"))
                print_progress(reported_bytes, expected_size or reported_bytes, done=True)
                break

            if line.startswith(("I (", "W (", "E (")):
                print(line)
                continue

            if not is_base64_payload_line(line):
                print(f"Skipping non-Base64 serial line during transfer: {line}")
                continue

            b64_lines.append(line)
            received_encoded_bytes += len(line)
            if expected_size is not None:
                estimated_received = min((received_encoded_bytes * 3) // 4, expected_size)
                print_progress(estimated_received, expected_size)

    if expected_size is None or reported_bytes is None:
        raise RuntimeError("Transfer did not include complete BEGIN_FILE/END_FILE markers")

    encoded = "".join(b64_lines)
    try:
        decoded = base64.b64decode(encoded, validate=True)
    except Exception as exc:
        raise RuntimeError("Failed to decode Base64 transfer") from exc

    if len(decoded) != expected_size:
        raise RuntimeError(
            f"Decoded size mismatch: got {len(decoded)} bytes, expected {expected_size}"
        )

    if reported_bytes != expected_size:
        raise RuntimeError(
            f"Device byte count mismatch: END_FILE reported {reported_bytes}, "
            f"BEGIN_FILE expected {expected_size}"
        )

    output.write_bytes(decoded)
    return output


def main() -> int:
    args = parse_args()
    csv_output: Path | None = None
    report: list[str] = []
    try:
        output = download_file(args)
        if not args.no_csv:
            csv_output = csv_path_for_bin(output, args.csv_output)
            report = convert_bin_to_csv(output, csv_output)
    except Exception as exc:
        print(f"Download failed: {exc}", file=sys.stderr)
        return 1

    print(f"Saved: {output}")
    if csv_output:
        print(f"Saved CSV: {csv_output}")
        for line in report:
            print(f"  {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
