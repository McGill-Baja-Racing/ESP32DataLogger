#!/usr/bin/env python3
"""Make NMEA replay files for the master's GPS input and play them through a USB-serial adapter.

Usage:
    python3 master-node/tools/gps_replay.py make full.nmea --seconds 18000
    python3 master-node/tools/gps_replay.py play full.nmea /dev/cu.usbserial-XXXX
    python3 master-node/tools/gps_replay.py junk /dev/cu.usbserial-XXXX

"full" files start with three no-fix lines, then hold one of each malformed sentence the parser must reject.
"""
import argparse
import datetime as dt
import os
import sys
import time


def sentence(body):
    check = 0
    for ch in body:
        check ^= ord(ch)
    return f"${body}*{check:02X}"


def rmc(t, talker="GP", lat="4530.1234,N", lon="07334.5678,W", speed="12.5", year=None):
    date = t.strftime("%d%m") + (year or t.strftime("%y"))
    return sentence(f"{talker}RMC,{t:%H%M%S}.00,A,{lat},{lon},{speed},90.0,{date},,,A")


def no_fix(t):
    return sentence(f"GPRMC,{t:%H%M%S}.00,V,,,,,,,{t:%d%m%y},,,N")


def make(start, seconds, kind):
    t = start
    lines = []
    for i in range(seconds):
        if kind == "nofix" or (kind == "full" and i < 3):
            line = no_fix(t)
        elif kind == "full" and i == 10:
            line = rmc(t, talker="GN")
        elif kind == "full" and i == 11:
            good = rmc(t)
            line = good[:-2] + f"{int(good[-2:], 16) ^ 0xFF:02X}"
        elif kind == "full" and i == 12:
            line = sentence(f"GPGGA,{t:%H%M%S}.00,4530.1234,N,07334.5678,W,1,08,0.9,50.0,M,0.0,M,,")
        elif kind == "full" and i == 13:
            line = rmc(t, speed="")
        elif kind == "full" and i == 14:
            line = rmc(t, lat="4530.1234,")
        elif kind == "full" and i == 15:
            line = rmc(t, year="19")
        elif kind == "full" and i == 16:
            line = "$GPTXT," + "A" * 123
        elif kind == "full" and i == 17:
            good = rmc(t)
            line = good[:20] + "\0" + good[20:]
        else:
            line = rmc(t)
        lines.append(line + "\r\n")
        t += dt.timedelta(seconds=1)
    return "".join(lines).encode("latin-1")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("make", help="write a replay file")
    m.add_argument("file")
    m.add_argument("--kind", choices=["full", "fixes", "nofix"], default="full")
    m.add_argument("--start", default="2026-09-20T14:00:00", help="UTC of the first line")
    m.add_argument("--seconds", type=int, default=600, help="one line per second")
    p = sub.add_parser("play", help="send a replay file, one line per second")
    p.add_argument("file")
    p.add_argument("port")
    p.add_argument("--loop", action="store_true")
    j = sub.add_parser("junk", help="send random bytes at the full 9600 baud rate")
    j.add_argument("port")
    j.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()

    if args.command == "make":
        start = dt.datetime.fromisoformat(args.start)
        with open(args.file, "wb") as f:
            f.write(make(start, args.seconds, args.kind))
        return 0

    import serial
    port = serial.Serial(args.port, 9600)
    if args.command == "junk":
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            port.write(os.urandom(96))
            time.sleep(0.1)
        return 0
    lines = open(args.file, "rb").read().splitlines(keepends=True)
    while True:
        for line in lines:
            port.write(line)
            print(line.decode("latin-1").rstrip().replace("\0", "<NUL>"), flush=True)
            time.sleep(1)
        if not args.loop:
            return 0


if __name__ == "__main__":
    sys.exit(main())
