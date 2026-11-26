#!/usr/bin/env python3
"""Logged bench console. Does not flash/reset or send commands on connection.

Requires pyserial (available in PlatformIO's Python environment). Input is an
allowlisted firmware command, or :quit. Motion requires --allow-three-revolutions.
The brake state-sequence test requires --allow-brake-probe.
"""
import argparse
import datetime
import json
import re
import sys
import threading
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--allow-three-revolutions", action="store_true")
    mode.add_argument("--original-test2", action="store_true", help="Original comparison firmware: only allow its explicit 2 start key")
    mode.add_argument("--allow-state-steps", action="store_true", help="Allow one explicitly requested controlword at a time")
    mode.add_argument("--allow-brake-probe", action="store_true", help="Allow zero-velocity brake state test (enables servo torque) and disable retry")
    args = parser.parse_args()
    allowed = {"help", "status", "servo ping", "servo inspect", "servo errors", "stop", "mode manual", "test list"}
    if args.allow_brake_probe:
        allowed.update({"servo brake-test", "servo brake-restore"})
    if args.allow_three_revolutions:
        allowed.add("test run three-revolutions")
        allowed.add("servo reset-fault")
        allowed.add("servo hold-test")
    if args.original_test2:
        allowed = {"2"}
    if args.allow_state_steps:
        allowed.update({"servo cw 0x0000", "servo cw 0x0006", "servo cw 0x0007", "servo cw 0x000F"})
        allowed.add("servo velocity-zero")
    lock = threading.Lock()
    done = threading.Event()
    link = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1, exclusive=True)
    link.dtr = False
    link.rts = False
    link.port = args.port
    with open(args.log, "x", buffering=1) as log:
        def record(kind, data):
            with lock:
                log.write(json.dumps({"time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                                      "monotonic": time.monotonic(), "kind": kind,
                                      "hex": data.hex(), "text": data.decode("utf-8", errors="replace")}) + "\n")

        link.open()
        print(f"Connected {args.port}; DTR/RTS deasserted; no startup command. Log: {args.log}", flush=True)

        def receive():
            try:
                while not done.is_set():
                    data = link.read(max(1, min(link.in_waiting, 4096)))
                    if data:
                        record("rx", data)
                        sys.stdout.write(data.decode("utf-8", errors="replace"))
                        sys.stdout.flush()
            except serial.SerialException as error:
                if not done.is_set():
                    print(f"\nSerial RX failed: {error}", flush=True)
                    done.set()

        reader = threading.Thread(target=receive, daemon=True)
        reader.start()
        try:
            for line in sys.stdin:
                command = line.strip()
                if command == ":quit":
                    break
                read_command = not args.original_test2 and re.fullmatch(r"servo read (?:0[xX][0-9a-fA-F]+|[0-9]+) (?:0[xX][0-9a-fA-F]+|[0-9]+) u(?:8|16|32)", command)
                if command not in allowed and not read_command:
                    print("Refused: command outside this session's bench allowlist.", flush=True)
                    continue
                if done.is_set():
                    break
                data = (command + "\n").encode("ascii")
                record("tx", data)
                link.write(data)
                link.flush()
        finally:
            done.set()
            reader.join(timeout=1)
            link.close()


if __name__ == "__main__":
    main()
