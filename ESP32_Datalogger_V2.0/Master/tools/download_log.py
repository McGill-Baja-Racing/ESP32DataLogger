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
import re
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
    try:
        output = download_file(args)
    except Exception as exc:
        print(f"Download failed: {exc}", file=sys.stderr)
        return 1

    print(f"Saved: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
