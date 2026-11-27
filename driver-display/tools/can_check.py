#!/usr/bin/env python3
"""Bench check for the driver display's MCP2515 CAN module on the Raspberry Pi.

Run on the Pi as root:  sudo python3 can_check.py

Needs only Python 3 and `ip` (iproute2). can-utils is not required.

Steps:
  1. Setup    /boot/firmware/config.txt, SPI device, mcp251x driver, can0, dmesg.
  2. Loopback The MCP2515 sends frames to itself (internal loopback, the
              transceiver is not used). Proves SPI (CS, SCK, MOSI, MISO), the
              crystal and the INT line, and checks that every byte comes back.
  3. Listen   Listen-only on the bus. Prints "NO DATA" when nothing arrives,
              otherwise decodes the DAQ frames (beacon, sensors, health...).
  4. TX       Only with --tx. Sends one frame in normal mode. With no other node on the bus,
              nobody acknowledges it, so a working transceiver ends in
              ERROR-PASSIVE. Bit errors (BUS-OFF) mean the controller cannot
              read back what it sends: transceiver not powered, in standby,
              or TX/RX wiring. Skipped when the listen step heard traffic or
              had to fall back to another bitrate (a wrong crystal hears nothing
              on the car's bus, yet its frames would disturb it).
              Runs at 125 kbit/s; keep jumper J1 (120 ohm) fitted on the bench.
              Never use --tx on the car: it puts frames on the bus.

can0 is left down at the end.
"""

import argparse
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import time

IFACE = "can0"
CONFIG_TXT = "/boot/firmware/config.txt"
SPI_DEVICE = "/sys/bus/spi/devices/spi0.0"
CAR_BITRATE = 1_000_000  # protocol/app_protocol.h CAN_BITRATE
BENCH_BITRATE = 500_000
TX_TEST_BITRATE = 125_000  # slow, so an unterminated bench bus still reads back cleanly

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_EFF_MASK = 0x1FFFFFFF
CAN_SFF_MASK = 0x7FF
CAN_ERR_MASK = 0x1FFFFFFF
CAN_FRAME = struct.Struct("=IB3x8s")
SOL_CAN_RAW = getattr(socket, "SOL_CAN_RAW", 101)
CAN_RAW_ERR_FILTER = getattr(socket, "CAN_RAW_ERR_FILTER", 2)

USE_COLOR = sys.stdout.isatty()
results = []  # (level, step, message)


# ---------------------------------------------------------------- output

def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if USE_COLOR else text


LEVEL_STYLE = {"PASS": "32", "FAIL": "31;1", "WARN": "33", "INFO": "36", "DATA": "35;1"}


def report(level, step, message):
    results.append((level, step, message))
    print(f"  [{_c(LEVEL_STYLE[level], level.ljust(4))}] {message}")


def detail(message):
    print(f"         {message}")


def header(title):
    print()
    print(_c("1", f"== {title} =="))


# ---------------------------------------------------------------- helpers

def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def ip_link(*args):
    code, out = run(["ip", "link", "set", IFACE, *args])
    if code != 0:
        raise RuntimeError(f"ip link set {IFACE} {' '.join(args)}: {out}")


def configure(bitrate, loopback=False, listen_only=False):
    ip_link("down")
    ip_link("type", "can", "bitrate", str(bitrate),
            "loopback", "on" if loopback else "off",
            "listen-only", "on" if listen_only else "off",
            "restart-ms", "0")
    ip_link("up")
    time.sleep(0.1)


def link_info():
    """State, clock, bitrate, ctrlmode and error statistics of can0."""
    info = {"state": None, "clock": None, "bitrate": None, "ctrlmode": [], "xstats": {}}
    code, out = run(["ip", "-j", "-d", "-s", "link", "show", "dev", IFACE])
    if code == 0:
        try:
            link = json.loads(out)[0].get("linkinfo", {})
            data = link.get("info_data", {})
            info["state"] = data.get("state")
            info["clock"] = data.get("clock")
            info["bitrate"] = data.get("bittiming", {}).get("bitrate")
            info["ctrlmode"] = data.get("ctrlmode", [])
            info["xstats"] = link.get("info_xstats", {}) or {}
        except (ValueError, IndexError, AttributeError):
            pass
    if info["state"] is None:  # older iproute2 without JSON details
        code, out = run(["ip", "-d", "link", "show", "dev", IFACE])
        if m := re.search(r"\bstate (ERROR-\w+|BUS-OFF|STOPPED|SLEEPING)", out):
            info["state"] = m.group(1)
        if m := re.search(r"\bclock (\d+)", out):
            info["clock"] = int(m.group(1))
        if m := re.search(r"\bbitrate (\d+)", out):
            info["bitrate"] = int(m.group(1))
    return info


def packet_counters():
    def read(name):
        try:
            with open(f"/sys/class/net/{IFACE}/statistics/{name}") as f:
                return int(f.read())
        except OSError:
            return 0
    return {n: read(n) for n in ("rx_packets", "tx_packets", "rx_errors",
                                 "tx_errors", "rx_dropped", "rx_over_errors")}


def irq_count():
    """Interrupts taken by the MCP2515 (GPIO 25), or None if not found."""
    try:
        with open("/proc/interrupts") as f:
            for line in f:
                if "spi0.0" in line or "mcp251" in line:
                    fields = line.split()
                    return sum(int(x) for x in fields[1:] if x.isdigit())
    except OSError:
        pass
    return None


def open_socket(errors=True):
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    if errors:
        sock.setsockopt(SOL_CAN_RAW, CAN_RAW_ERR_FILTER, struct.pack("=I", CAN_ERR_MASK))
    sock.bind((IFACE,))
    return sock


def send(sock, can_id, data, extended=False):
    raw_id = (can_id & CAN_EFF_MASK) | CAN_EFF_FLAG if extended else can_id & CAN_SFF_MASK
    sock.send(CAN_FRAME.pack(raw_id, len(data), data.ljust(8, b"\x00")))


def recv(sock, timeout):
    """One frame as (id, extended, is_error, data), or None on timeout."""
    ready, _, _ = select.select([sock], [], [], max(timeout, 0))
    if not ready:
        return None
    raw = sock.recv(16)
    raw_id, length, data = CAN_FRAME.unpack(raw)
    extended = bool(raw_id & CAN_EFF_FLAG)
    is_error = bool(raw_id & CAN_ERR_FLAG)
    can_id = raw_id & (CAN_EFF_MASK if extended or is_error else CAN_SFF_MASK)
    return can_id, extended, is_error, data[:length]


ERR_CLASSES = {0x1: "TX timeout", 0x2: "arbitration lost", 0x4: "controller",
               0x8: "protocol violation", 0x10: "transceiver", 0x20: "no ACK",
               0x40: "BUS-OFF", 0x80: "bus error", 0x100: "controller restarted"}
CTRL_BITS = {0x01: "RX overflow", 0x02: "TX overflow", 0x04: "RX warning",
             0x08: "TX warning", 0x10: "RX passive", 0x20: "TX passive",
             0x40: "back to error-active"}


def describe_error(can_id, data):
    parts = [name for bit, name in ERR_CLASSES.items() if can_id & bit]
    if can_id & 0x4 and len(data) > 1:
        parts += [name for bit, name in CTRL_BITS.items() if data[1] & bit]
    if len(data) >= 8 and (data[6] or data[7]):
        parts.append(f"TEC={data[6]} REC={data[7]}")
    return ", ".join(parts) or f"error 0x{can_id:X}"


# ---------------------------------------------------------------- DAQ decoding

SENSORS = {
    0x0B1: ("front brake pressure", "psi", 1), 0x0B2: ("rear brake pressure", "psi", 1),
    0x0B3: ("accel X", "mg", 3), 0x0B4: ("accel Y", "mg", 3), 0x0B5: ("accel Z", "mg", 3),
    0x0B6: ("gyro X", "mdeg/s", 3), 0x0B7: ("gyro Y", "mdeg/s", 3), 0x0B8: ("gyro Z", "mdeg/s", 3),
    0x0B9: ("bearing RPM", "rpm", 4), 0x0BA: ("generic ADC", "mV", 6),
    0x0BB: ("engine RPM", "rpm", 5), 0x0BC: ("engine spark", "", 5),
    0x0BD: ("front lin pot 1", "mm", 7), 0x0BE: ("front lin pot 2", "mm", 7),
    0x0BF: ("rear lin pot 1", "mm", 8), 0x0C9: ("rear lin pot 2", "mm", 8),
}
STATE_REASONS = {1: "boot", 2: "stop", 3: "start", 4: "CAN recovery", 5: "beacon sync"}


def describe_frame(can_id, extended, data):
    """(name, decoded value) for a DAQ frame."""
    if extended:
        return "extended ID (not DAQ)", data.hex(" ")
    if can_id == 0x0A0:
        return "master STOP", data.hex(" ") or "(empty)"
    if can_id == 0x0A1:
        return "master START", data.hex(" ") or "(empty)"
    if can_id == 0x0A2 and len(data) == 8:
        value = int.from_bytes(data, "little")
        recording = "recording" if value >> 63 else "idle"
        return "master beacon", f"{recording}, master up {(value & ~(1 << 63)) / 1e6:.1f} s"
    if can_id in SENSORS and len(data) == 8:
        name, unit, node = SENSORS[can_id]
        value = int.from_bytes(data[:4], "little", signed=True)
        stamp = int.from_bytes(data[4:], "little")
        return f"node {node} {name}", f"{value} {unit} @ {stamp} ms".replace("  ", " ")
    if 0x0C1 <= can_id <= 0x0C8 and len(data) >= 2:
        state = {0: "idle", 1: "active"}.get(data[0], f"state {data[0]}")
        reason = STATE_REASONS.get(data[1], f"reason {data[1]}")
        return f"node {can_id - 0x0C0} state", f"{state} ({reason})"
    if can_id == 0x500 and len(data) >= 4:
        return "AiM EVO4S", f"{int.from_bytes(data[:2], 'little')} rpm, counter {int.from_bytes(data[2:4], 'little')}"
    if 0x700 <= can_id <= 0x703:
        return "GPS / AiM record", data.hex(" ")
    for base, name in ((0x0D0, "status"), (0x0E0, "event"), (0x0F0, "diagnostics"),
                       (0x080, "update"), (0x090, "identity")):
        if base <= can_id <= base + 8:
            return f"node {can_id - base} {name}", data.hex(" ")
    if can_id in (0x0A4, 0x0A5):
        return "OTA update", data.hex(" ")
    return "unknown", data.hex(" ")


# ---------------------------------------------------------------- steps

def step_setup():
    header("1. Setup")
    ok = True

    if not sys.platform.startswith("linux"):
        report("FAIL", "setup", "Run this on the Raspberry Pi (Linux), not on this computer.")
        return False

    try:
        with open(CONFIG_TXT) as f:
            lines = [l.strip() for l in f if l.strip() and not l.strip().startswith("#")]
    except OSError as e:
        lines = []
        report("WARN", "setup", f"Cannot read {CONFIG_TXT}: {e}")
    if lines:
        if "dtparam=spi=on" in lines:
            report("PASS", "setup", "config.txt: dtparam=spi=on")
        else:
            report("WARN", "setup", "config.txt: no active 'dtparam=spi=on' line")
        overlay = next((l for l in lines if l.startswith("dtoverlay=mcp2515")), None)
        if overlay:
            report("PASS", "setup", f"config.txt: {overlay}")
            params = dict(p.split("=", 1) for p in overlay.split(",")[1:] if "=" in p)
            if "oscillator" not in params:
                report("WARN", "setup", "overlay has no oscillator=..., it must match the module's crystal")
            if params.get("interrupt", "25") != "25":
                report("WARN", "setup", f"overlay interrupt={params['interrupt']}, the schematic wires INT to GPIO 25")
        else:
            report("FAIL", "setup", "config.txt: no 'dtoverlay=mcp2515-can0,...' line. "
                   "Add it (see hardware/can-module/README.md) and reboot.")
            ok = False

    if os.path.exists(SPI_DEVICE):
        driver = os.path.basename(os.path.realpath(f"{SPI_DEVICE}/driver")) \
            if os.path.exists(f"{SPI_DEVICE}/driver") else None
        if driver == "mcp251x":
            report("PASS", "setup", "spi0.0 is bound to the mcp251x driver")
        else:
            report("FAIL", "setup", f"spi0.0 exists but the driver is {driver or 'not bound'}: "
                   "the MCP2515 did not answer on SPI at boot")
            detail("Wire by PHYSICAL pin: CS 24, SCK 23, SI 19, SO 21, INT 22 (BCM GPIO 8, 11, 10, 9, 25).")
            detail("WiringPi diagrams call other pins 'GPIO 8..11, 25' (physical 3, 26, 24, 5, 37).")
            ok = False
    else:
        report("FAIL", "setup", "no SPI device spi0.0: SPI or the overlay is not active")
        ok = False

    code, out = run(["dmesg"])
    if code != 0:
        out = ""
        report("INFO", "setup", "dmesg not readable without root, skipped")
    mcp_lines = [l for l in out.splitlines() if "mcp251" in l.lower()]
    for line in mcp_lines[-5:]:
        detail(line)
    if any("success" in l.lower() for l in mcp_lines):
        report("PASS", "setup", "dmesg: MCP2515 successfully initialized")
    elif mcp_lines:
        report("FAIL", "setup", "dmesg reports an MCP2515 problem (see above). "
               "Check the SPI wiring (CS, SCK, SI, SO) and the module's supply.")
        ok = False

    if not os.path.exists(f"/sys/class/net/{IFACE}"):
        report("FAIL", "setup", f"no {IFACE} network interface")
        return False
    report("PASS", "setup", f"{IFACE} exists")
    if os.geteuid() != 0:
        report("FAIL", "setup", "Needs root to configure can0. Run: sudo python3 can_check.py")
        return False

    info = link_info()
    if info["clock"]:
        crystal = info["clock"] * 2  # mcp251x reports half the oscillator frequency
        report("INFO", "setup", f"driver crystal setting: {crystal / 1e6:g} MHz")
        if crystal < 16_000_000:
            detail("An 8 MHz crystal cannot run the car's 1 Mbit/s bus (800 kbit/s at most).")
    return ok


def step_loopback():
    header("2. Loopback (SPI, crystal, INT; transceiver not used)")
    try:
        configure(BENCH_BITRATE, loopback=True)
    except RuntimeError as e:
        report("FAIL", "loopback", str(e))
        return False

    tests = [
        (0x123, False, bytes.fromhex("DEADBEEF")),
        (0x000, False, b""),
        (0x7FF, False, b"\xff" * 8),
        (0x555, False, b"\x55" * 8),
        (0x2AA, False, b"\xaa" * 8),
        (0x100, False, b"\x00" * 8),
        (0x0B1, False, bytes.fromhex("DC050000E8030000")),
        (0x1ABCDE5A, True, bytes(range(1, 9))),
        (0x00000001, True, bytes([0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01])),
    ]
    rx, tx = open_socket(), open_socket(errors=False)
    before, irq_before = packet_counters(), irq_count()
    seen = {i: [] for i in range(len(tests))}
    unexpected, errors = [], []

    for index, (can_id, ext, data) in enumerate(tests):
        try:
            send(tx, can_id, data, ext)
        except OSError as e:
            report("FAIL", "loopback", f"send 0x{can_id:X} failed: {e}")
            break
        deadline = time.monotonic() + 0.3
        while time.monotonic() < deadline:
            frame = recv(rx, deadline - time.monotonic())
            if frame is None:
                break
            r_id, r_ext, r_err, r_data = frame
            if r_err:
                errors.append(describe_error(r_id, r_data))
                continue
            match = next((i for i, t in enumerate(tests) if t[0] == r_id and t[1] == r_ext), None)
            if match is None:
                unexpected.append(frame)
            else:
                seen[match].append(r_data)
    while (frame := recv(rx, 0.2)) is not None:  # late arrivals
        r_id, r_ext, r_err, r_data = frame
        match = next((i for i, t in enumerate(tests) if t[0] == r_id and t[1] == r_ext), None)
        if r_err:
            errors.append(describe_error(r_id, r_data))
        elif match is None:
            unexpected.append(frame)
        else:
            seen[match].append(r_data)
    rx.close()
    tx.close()

    after, irq_after = packet_counters(), irq_count()
    tx_done = after["tx_packets"] - before["tx_packets"]
    rx_done = after["rx_packets"] - before["rx_packets"]
    n = len(tests)
    ok = True

    missing = [tests[i] for i, got in seen.items() if not got]
    corrupt = [(tests[i], got) for i, got in seen.items() if any(d != tests[i][2] for d in got)]
    if missing:
        ok = False
        report("FAIL", "loopback", f"{len(missing)}/{n} frames never came back "
               f"(sent {tx_done}, received {rx_done} by the chip)")
        if tx_done == 0:
            detail("Nothing completed: check INT (GPIO 25, pin 22) and the crystal.")
    if corrupt:
        ok = False
        report("FAIL", "loopback", f"{len(corrupt)} frames came back with different bytes: "
               "SPI corruption, check SO/SI and the 3.3 V levels")
        for (can_id, _, want), got in corrupt[:4]:
            detail(f"0x{can_id:X}: sent {want.hex(' ') or '-'}, got {[g.hex(' ') for g in got]}")
    if unexpected:
        ok = False
        report("FAIL", "loopback", f"{len(unexpected)} frames with unexpected IDs (corrupted ID bits?)")
        for f_id, f_ext, _, f_data in unexpected[:4]:
            detail(f"0x{f_id:X}{' ext' if f_ext else ''}: {f_data.hex(' ')}")
    if rx_done < n:
        ok = False
        report("FAIL", "loopback", f"the chip received only {rx_done}/{n} of its own frames")
    if errors:
        report("WARN", "loopback", f"error frames: {'; '.join(sorted(set(errors)))}")
    if ok:
        report("PASS", "loopback", f"{n}/{n} frames sent and received by the MCP2515, every byte identical "
               "(standard + extended IDs, 0 to 8 bytes, 0x55/0xAA patterns)")

    if irq_before is not None and irq_after is not None:
        if irq_after > irq_before:
            report("PASS", "loopback", f"INT line works: {irq_after - irq_before} interrupts on GPIO 25")
        else:
            report("FAIL", "loopback", "no interrupt from the MCP2515: check INT (pin 22) and its divider")
            ok = False
    return ok


def step_listen(bitrate, seconds):
    header(f"3. Listen to the bus ({seconds:g} s, listen-only)")
    used = bitrate
    try:
        configure(bitrate, listen_only=True)
    except RuntimeError:
        report("WARN", "listen", f"cannot set {bitrate} bit/s (crystal too slow?), "
               f"listening at {BENCH_BITRATE} instead: car frames cannot be decoded")
        used = BENCH_BITRATE
        try:
            configure(used, listen_only=True)
        except RuntimeError as e:
            report("FAIL", "listen", str(e))
            return None
    report("INFO", "listen", f"listening at {used} bit/s")
    fallback = used != bitrate

    sock = open_socket()
    frames, errors = {}, []
    start = time.monotonic()
    deadline = start + seconds
    while (remaining := deadline - time.monotonic()) > 0:
        frame = recv(sock, remaining)
        if frame is None:
            continue
        can_id, ext, is_err, data = frame
        if is_err:
            errors.append(describe_error(can_id, data))
            continue
        entry = frames.setdefault((can_id, ext), {"count": 0, "data": b""})
        entry["count"] += 1
        entry["data"] = data
    sock.close()
    elapsed = time.monotonic() - start
    info = link_info()

    if not frames:
        report("DATA", "listen", f"NO DATA: no frame in {elapsed:.1f} s "
               "(expected while the module is not on the DAQ bus)")
    else:
        total = sum(e["count"] for e in frames.values())
        report("DATA", "listen", f"{total} frames from {len(frames)} IDs in {elapsed:.1f} s")
        if any(cid == 0x0A2 for cid, _ in frames):
            report("PASS", "listen", "master beacon present: the DAQ master is on the bus")
        print(f"         {'ID':>9}  {'Hz':>6}  {'source':<28} last value")
        for (can_id, ext), e in sorted(frames.items()):
            name, value = describe_frame(can_id, ext, e["data"])
            ident = f"0x{can_id:08X}" if ext else f"0x{can_id:03X}"
            print(f"         {ident:>9}  {e['count'] / elapsed:6.1f}  {name:<28} {value}")
    if errors:
        report("WARN", "listen", f"error frames: {'; '.join(sorted(set(errors)))}")
    if info["state"] and info["state"] != "ERROR-ACTIVE":
        report("WARN", "listen", f"controller state {info['state']}")
    counters = packet_counters()
    if counters["rx_over_errors"] or counters["rx_dropped"]:
        report("WARN", "listen", f"frames lost since boot: overruns {counters['rx_over_errors']}, "
               f"dropped {counters['rx_dropped']}")
    return {"frames": len(frames), "errors": len(errors), "fallback": fallback}


def step_transmit(bitrate):
    header(f"4. Transceiver (normal mode, one frame at {bitrate // 1000} kbit/s)")
    try:
        configure(bitrate)
    except RuntimeError as e:
        report("FAIL", "tx", str(e))
        return False

    sock = open_socket()
    before = packet_counters()
    xstats_before = link_info()["xstats"]
    try:
        send(sock, 0x7FE, b"\xca\xfe")
    except OSError as e:
        report("FAIL", "tx", f"send failed: {e}")
        sock.close()
        return False

    errors, state = [], None
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        frame = recv(sock, 0.1)
        if frame and frame[2]:
            errors.append(describe_error(frame[0], frame[3]))
        state = link_info()["state"]
        if state == "BUS-OFF":
            break
    sock.close()
    info = link_info()
    state = info["state"] or state
    acked = packet_counters()["tx_packets"] > before["tx_packets"]
    xs = {k: info["xstats"].get(k, 0) - xstats_before.get(k, 0) for k in info["xstats"]}
    went_passive = state == "ERROR-PASSIVE" or xs.get("error_passive", 0) > 0
    went_bus_off = state == "BUS-OFF" or xs.get("bus_off", 0) > 0
    ip_link("down")  # stops the MCP2515 retrying the frame

    report("INFO", "tx", f"controller state after 1 s: {state}")
    if errors:
        detail(f"error frames: {'; '.join(sorted(set(errors)))}")
    if acked:
        report("PASS", "tx", "the frame was acknowledged: the transceiver works and another node is on the bus")
        return True
    if went_bus_off:
        report("FAIL", "tx", "BUS-OFF: the MCP2515 does not read back what it sends (bit errors)")
        detail("Check: J1 (120 ohm) fitted, since nothing else terminates the bench bus;")
        detail("transceiver supply (a TJA1050 needs 5 V, an SN65HVD230 runs on 3.3 V);")
        detail("its S/Rs pin (high = silent/standby); TXCAN/RXCAN between MCP2515 and transceiver.")
        return False
    if went_passive:
        report("PASS", "tx", "ERROR-PASSIVE without bus-off: the transceiver drives the bus and reads it back; "
               "nobody acknowledged (expected with no other node)")
        return True
    report("FAIL", "tx", "the controller never transmitted (no error, no ACK)")
    detail("The bus never looks idle: CANH/CANL shorted together, or the transceiver's RXD stuck low.")
    return False


# ---------------------------------------------------------------- main

def transmit_decision(want_tx, heard, loop_ok):
    """(run step 4?, why not). Transmits only when asked, on a quiet bus, at the asked bitrate."""
    if not want_tx:
        return False, "skipped: transmitting needs --tx (never on the car)"
    if heard is None:
        return False, "skipped: the listen step failed"
    if heard["fallback"]:
        return False, "skipped: listening fell back to another bitrate, so the bus may be busy at a speed we cannot hear"
    if heard["frames"] or heard["errors"]:
        return False, "skipped: traffic or errors on the bus, not transmitting onto it"
    if not loop_ok:
        return False, "skipped: fix the loopback step first"
    return True, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bitrate", type=int, default=CAR_BITRATE,
                        help=f"bus bitrate for the listen step (default {CAR_BITRATE}, the car's)")
    parser.add_argument("--listen", type=float, default=5.0, metavar="SECONDS",
                        help="how long to listen (default 5)")
    parser.add_argument("--tx", action="store_true",
                        help="run the transmit step on a bench bus (never on the car)")
    args = parser.parse_args()

    print(_c("1", "Driver display CAN check") + f"  ({time.strftime('%Y-%m-%d %H:%M:%S')})")
    try:
        if step_setup():
            loop_ok = step_loopback()
            heard = step_listen(args.bitrate, args.listen)
            run, reason = transmit_decision(args.tx, heard, loop_ok)
            if run:
                step_transmit(TX_TEST_BITRATE)
            else:
                header("4. Transceiver")
                report("INFO", "tx", reason)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if sys.platform.startswith("linux") and os.path.exists(f"/sys/class/net/{IFACE}"):
            run(["ip", "link", "set", IFACE, "down"])

    header("Summary")
    counts = {lvl: sum(1 for r in results if r[0] == lvl) for lvl in LEVEL_STYLE}
    print("  " + "  ".join(f"{_c(LEVEL_STYLE[l], l)} {counts[l]}" for l in LEVEL_STYLE if counts[l]))
    for level, step, message in results:
        if level in ("FAIL", "WARN"):
            print(f"  {_c(LEVEL_STYLE[level], level)} [{step}] {message}")
    if not counts["FAIL"]:
        print(_c("32;1", "  All checks passed."))
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
