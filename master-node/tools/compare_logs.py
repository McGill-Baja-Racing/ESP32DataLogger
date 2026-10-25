#!/usr/bin/env python3
"""Compare two master SD logs of the same bench test, for example the C build against the kernel.

Usage:
    python3 tools/compare_logs.py runs/A/4.7/sd/log_0003.bin runs/B/4.7/sd/log_0001.bin
    python3 tools/compare_logs.py A.bin B.bin --from 5 --to 65

Per-signal measures and skew use the --from/--to window; health uses the whole log. Both logs need
their .bin.utc next to them unless --no-utc is given. --replay also requires identical GPS values.
Exits with 1 when any check fails.
"""

from __future__ import annotations

import argparse
import statistics
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from decode_log import RECORD_SIZE_BYTES, SIGNAL_METADATA, signed_u32

NODE_STATUS_BASE = 0x0D0
NODE_EVENT_BASE = 0x0E0
NODE_DIAGNOSTICS_BASE = 0x0F0
NODE_SLOT_COUNT = 7

# Matches health_code_t in protocol/app_protocol.h.
HEALTH_CODES = [
    "sensor_init", "sensor_read", "sensor_dead", "samples_missed", "samples_dropped",
    "bad_timestamp", "no_time_sync", "can_tx", "can_rx_overflow", "can_no_ack",
    "can_bus_error", "can_passive", "can_bus_off", "sd_write", "gps_no_data",
    "gps_no_fix", "event_lost", "boot", "adc_uncalibrated", "node_offline",
]
HEALTH_NAMES = ["ok", "warning", "error", "critical"]
# Matches health_error_t.
HEALTH_ERRORS = [
    "ESP_OK", "ESP_FAIL", "ESP_ERR_NO_MEM", "ESP_ERR_INVALID_ARG", "ESP_ERR_INVALID_STATE",
    "ESP_ERR_NOT_FOUND", "ESP_ERR_TIMEOUT", "ESP_ERR_INVALID_RESPONSE", "other",
]
# Indexed by esp_reset_reason_t.
RESET_NAMES = [
    "unknown", "power_on", "external", "software", "panic", "interrupt_watchdog",
    "task_watchdog", "watchdog", "deep_sleep", "brownout", "sdio", "usb", "jtag",
    "efuse", "power_glitch", "cpu_lockup",
]
GPS_IDS = (0x700, 0x701, 0x702)

# Largest difference allowed between the A and B means, in the signal's units.
MEAN_TOLERANCE = {
    0x0B1: 30, 0x0B2: 16,
    0x0B3: 30, 0x0B4: 30, 0x0B5: 30,
    0x0B6: 500, 0x0B7: 500, 0x0B8: 500,
    0x0B9: 1, 0x0BA: 20, 0x0BB: 1, 0x0BC: 0,
}
TIMING_SLACK_MS = 2


def health_base(can_id: int) -> int | None:
    for base in (NODE_STATUS_BASE, NODE_EVENT_BASE, NODE_DIAGNOSTICS_BASE):
        if base <= can_id < base + NODE_SLOT_COUNT:
            return base
    return None


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


@dataclass
class Signal:
    count: int = 0
    rate_hz: float = 0.0
    gap_median: int = 0
    gap_p99: int = 0
    gap_max: int = 0
    backwards: int = 0
    mean: float = 0.0
    std: float = 0.0
    minimum: int = 0
    maximum: int = 0


@dataclass
class Log:
    signals: dict[int, Signal] = field(default_factory=dict)
    events: dict[tuple[int, str, int, str], int] = field(default_factory=dict)
    worst_health: dict[int, int] = field(default_factory=dict)
    faults: dict[int, int] = field(default_factory=dict)
    diagnostics: dict[int, tuple[int, int, int, int]] = field(default_factory=dict)
    gps: list[tuple[int, int]] = field(default_factory=list)
    skew_ms: int = 0
    bad_timestamps: int = 0
    utc: str = "missing"
    utc_ok: bool = False


def analyse(path: Path, start_s: float | None, end_s: float | None) -> Log:
    data = path.read_bytes()
    records = []
    for offset in range(0, len(data) - RECORD_SIZE_BYTES + 1, RECORD_SIZE_BYTES):
        can_id, packed = struct.unpack_from("<qQ", data, offset)
        records.append((can_id & 0x7FF, packed >> 32, signed_u32(packed)))

    log = Log()
    for can_id, timestamp, value in records:
        if timestamp >= 1 << 31:
            log.bad_timestamps += 1
            continue
        raw = value & 0xFFFFFFFF
        base = health_base(can_id)
        node = can_id - base if base is not None else None
        if base == NODE_EVENT_BASE:
            code = raw & 0x1F
            error = (raw >> 5) & 0xF
            name = HEALTH_CODES[code] if code < len(HEALTH_CODES) else f"code_{code}"
            key = (node, name, (raw >> 9) & 0xFF,
                   HEALTH_ERRORS[error] if error < len(HEALTH_ERRORS) else f"error_{error}")
            log.events[key] = log.events.get(key, 0) + (raw >> 23)
        elif base == NODE_STATUS_BASE:
            log.worst_health[node] = max(log.worst_health.get(node, 0), (raw >> 24) & 0x3)
            log.faults[node] = log.faults.get(node, 0) | (raw & 0xFFFFFF)
        elif base == NODE_DIAGNOSTICS_BASE:
            log.diagnostics[node] = (raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF, raw >> 24)
        elif can_id in GPS_IDS:
            log.gps.append((can_id, value))

    # An event's timestamp is its last occurrence, so events are left out of timing.
    timed = [r for r in records if r[1] < 1 << 31 and health_base(r[0]) != NODE_EVENT_BASE]
    first = min((r[1] for r in timed), default=0)
    low = first + start_s * 1000 if start_s is not None else first
    high = first + end_s * 1000 if end_s is not None else max((r[1] for r in timed), default=0)
    newest = None
    for _, timestamp, _ in timed:
        if not low <= timestamp <= high:
            continue
        if newest is not None:
            log.skew_ms = max(log.skew_ms, newest - timestamp)
        newest = timestamp if newest is None else max(newest, timestamp)
    by_id: dict[int, list[tuple[int, int]]] = {}
    for can_id, timestamp, value in timed:
        if low <= timestamp <= high:
            by_id.setdefault(can_id, []).append((timestamp, value))
    seconds = max((high - low) / 1000, 0.001)
    for can_id, samples in by_id.items():
        values = [v for _, v in samples]
        steps = [b[0] - a[0] for a, b in zip(samples, samples[1:])]
        gaps = [s for s in steps if s >= 0] or [0]
        log.signals[can_id] = Signal(
            count=len(samples),
            rate_hz=len(samples) / seconds,
            gap_median=int(statistics.median(gaps)),
            gap_p99=percentile(gaps, 0.99),
            gap_max=max(gaps),
            backwards=sum(1 for s in steps if s < 0),
            mean=statistics.fmean(values),
            std=statistics.pstdev(values),
            minimum=min(values),
            maximum=max(values),
        )

    utc_path = path.with_name(path.name + ".utc")
    if utc_path.exists():
        utc_data = utc_path.read_bytes()
        utc = [u for (u,) in struct.iter_unpack("<q", utc_data[: len(utc_data) // 8 * 8])]
        offsets = [u - r[1] for u, r in zip(utc, records) if u > 0]
        spread = max(offsets) - min(offsets) if offsets else 0
        log.utc_ok = len(utc_data) == 8 * len(records) and spread <= TIMING_SLACK_MS
        log.utc = (f"{len(utc)} entries for {len(records)} records, {len(offsets)} synced, "
                   f"utc-timestamp spread {spread} ms")
    return log


class Report:
    def __init__(self) -> None:
        self.failures = 0

    def row(self, label: str, measure: str, a: object, b: object, ok: bool | None) -> None:
        result = "" if ok is None else "ok" if ok else "FAIL"
        self.failures += ok is False
        print(f"{label:<28} {measure:<12} {str(a):>14} {str(b):>14}  {result}")


def fault_names(bits: int | None) -> str:
    if bits is None:
        return "-"
    return " ".join(name for i, name in enumerate(HEALTH_CODES) if bits & (1 << i)) or "none"


def reset_name(reason: int) -> str:
    return RESET_NAMES[reason] if reason < len(RESET_NAMES) else str(reason)


def compare(a: Log, b: Log, check_utc: bool, replay: bool) -> int:
    report = Report()
    print(f"{'signal':<28} {'measure':<12} {'A':>14} {'B':>14}  result")
    for can_id in sorted(a.signals.keys() | b.signals.keys()):
        name = SIGNAL_METADATA.get(can_id, ("",))[0]
        label = f"0x{can_id:03X} {name}"
        sa, sb = a.signals.get(can_id), b.signals.get(can_id)
        if not sa or not sb:
            report.row(label, "count", sa.count if sa else 0, sb.count if sb else 0, False)
            continue
        report.row(label, "count", sa.count, sb.count,
                   abs(sb.count - sa.count) <= max(1, 0.01 * sa.count))
        report.row(label, "rate_hz", f"{sa.rate_hz:.2f}", f"{sb.rate_hz:.2f}", None)
        report.row(label, "gap_median", sa.gap_median, sb.gap_median, None)
        report.row(label, "gap_p99", sa.gap_p99, sb.gap_p99,
                   sb.gap_p99 <= sa.gap_p99 + TIMING_SLACK_MS)
        report.row(label, "gap_max", sa.gap_max, sb.gap_max, None)
        report.row(label, "backwards", sa.backwards, sb.backwards, sb.backwards <= sa.backwards)
        if health_base(can_id) is not None:
            continue
        tolerance = MEAN_TOLERANCE.get(can_id)
        report.row(label, "mean", f"{sa.mean:.2f}", f"{sb.mean:.2f}",
                   None if tolerance is None else abs(sb.mean - sa.mean) <= tolerance)
        if tolerance is None:
            std_ok = None
        elif sa.std > 0:
            std_ok = 0.5 <= sb.std / sa.std <= 2
        else:
            std_ok = sb.std <= tolerance
        report.row(label, "std", f"{sa.std:.2f}", f"{sb.std:.2f}", std_ok)
        report.row(label, "min..max", f"{sa.minimum}..{sa.maximum}",
                   f"{sb.minimum}..{sb.maximum}", None)

    print()
    report.row("all", "skew_ms", a.skew_ms, b.skew_ms, b.skew_ms <= a.skew_ms + TIMING_SLACK_MS)
    report.row("all", "bad_ts", a.bad_timestamps, b.bad_timestamps,
               b.bad_timestamps <= a.bad_timestamps)
    if check_utc:
        report.row("all", "utc", "ok" if a.utc_ok else "bad", "ok" if b.utc_ok else "bad",
                   a.utc_ok and b.utc_ok)
        print(f"utc A: {a.utc}\nutc B: {b.utc}")
    if replay:
        report.row("gps", "values", len(a.gps), len(b.gps), a.gps == b.gps)

    print()
    for key in sorted(a.events.keys() | b.events.keys()):
        node, name, subject, error = key
        ca, cb = a.events.get(key, 0), b.events.get(key, 0)
        report.row(f"node {node} {name} {error}", f"0x{subject:02X}", ca, cb,
                   ca > 0 and cb > 0 and abs(cb - ca) <= max(2, 0.1 * ca))
    for node in sorted(a.worst_health.keys() | b.worst_health.keys()):
        ha, hb = a.worst_health.get(node), b.worst_health.get(node)
        report.row(f"node {node}", "worst", HEALTH_NAMES[ha] if ha is not None else "-",
                   HEALTH_NAMES[hb] if hb is not None else "-", ha == hb)
    for node in sorted(a.faults.keys() | b.faults.keys()):
        fa, fb = a.faults.get(node), b.faults.get(node)
        result = "ok" if fa == fb else "FAIL"
        print(f"node {node} faults seen  {result}\n  A: {fault_names(fa)}\n  B: {fault_names(fb)}")
        report.failures += fa != fb
    for node in sorted(a.diagnostics.keys() | b.diagnostics.keys()):
        da, db = a.diagnostics.get(node), b.diagnostics.get(node)
        report.row(f"node {node}", "resets", da[2] if da else "-", db[2] if db else "-",
                   bool(da and db) and da[2] == db[2])
        report.row(f"node {node}", "last_reset", reset_name(da[3]) if da else "-",
                   reset_name(db[3]) if db else "-", bool(da and db) and da[3] == db[3])
        report.row(f"node {node}", "tx/rx errors", f"{da[0]}/{da[1]}" if da else "-",
                   f"{db[0]}/{db[1]}" if db else "-", None)

    print(f"\n{report.failures} check(s) failed")
    return 1 if report.failures else 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare two master SD .bin logs.")
    parser.add_argument("a", help="Reference log, for example the C build.")
    parser.add_argument("b", help="Log under test, for example the kernel build.")
    parser.add_argument("--from", dest="start", type=float,
                        help="Seconds after each log's first record where the steady window starts.")
    parser.add_argument("--to", dest="end", type=float,
                        help="Seconds after each log's first record where the steady window ends.")
    parser.add_argument("--no-utc", action="store_true",
                        help="Skip the .bin.utc check, for logs downloaded over Wi-Fi.")
    parser.add_argument("--replay", action="store_true",
                        help="Require identical GPS values, for runs fed by the same NMEA replay.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    paths = [Path(args.a), Path(args.b)]
    for path in paths:
        if not path.exists():
            print(f"Input file not found: {path}", file=sys.stderr)
            return 1
    a, b = (analyse(path, args.start, args.end) for path in paths)
    return compare(a, b, not args.no_utc, args.replay)


if __name__ == "__main__":
    raise SystemExit(main())
