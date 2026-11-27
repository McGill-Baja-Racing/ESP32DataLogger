#!/usr/bin/env python3
"""Pretend to be the DAQ on a virtual CAN bus, to test the display without the car.

Two modes:
  --replay LOG.bin   replay a recording copied from the master's SD card, at its
                     own pace (see docs/LOG_FORMAT.md), with synthetic beacons
  (default)          send fake but plausible values at the real rates

Examples, on the Pi:
  sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
  python3 tools/daq_sim.py vcan0
  python3 tools/daq_sim.py vcan0 --replay log_0001.bin --speed 2 --loop
  python3 tools/daq_sim.py vcan0 --stop-node 4 --after 10   # node 4 goes silent after 10 s

It transmits, so it refuses any interface not named vcan*: fake frames must never
reach the car's bus. --allow-real-interface overrides this, for example for can0
in MCP2515 loopback mode on the bench.

Frame layouts and IDs follow protocol/app_protocol.h and docs/PROTOCOL.md.
Python standard library only.
"""
import argparse
import math
import random
import socket
import struct
import sys
import time
from pathlib import Path

# IDs from protocol/app_protocol.h
STOP, START, BEACON = 0x0A0, 0x0A1, 0x0A2
STATE_BASE, STATUS_BASE, EVENT_BASE, DIAGNOSTICS_BASE = 0x0C0, 0x0D0, 0x0E0, 0x0F0
AIM_WIRE, AIM_RECORD = 0x500, 0x703
RECORDING_FLAG = 1 << 63
SENSOR_NODE = {
    0x0B1: 1, 0x0B2: 1,
    0x0B3: 3, 0x0B4: 3, 0x0B5: 3, 0x0B6: 3, 0x0B7: 3, 0x0B8: 3,
    0x0B9: 4, 0x0BA: 6, 0x0BB: 5, 0x0BC: 5,
    0x0BD: 7, 0x0BE: 7, 0x0BF: 8, 0x0C9: 8,
}
NODE_COUNT = 8
HEALTH_PAYLOAD = 4          # nodes send status and diagnostics without the master's timestamp
REASON_BOOT, REASON_START = 1, 3
DEFAULT_NODES = {1, 3, 4, 5, 7, 8}

CAN_FRAME = struct.Struct('=IB3x8s')


def node_of(can_id):
    """Node 1..8 that sends this ID, or 0."""
    if can_id in SENSOR_NODE:
        return SENSOR_NODE[can_id]
    for base in (STATE_BASE, STATUS_BASE, EVENT_BASE, DIAGNOSTICS_BASE):
        if base < can_id <= base + NODE_COUNT:
            return can_id - base
    return 0


def record_to_frames(log_id, payload, counter):
    """Wire frames for one logged record, as (id, bytes); [] for master-only records."""
    data = payload.to_bytes(8, 'little')
    if log_id in SENSOR_NODE:
        return [(log_id, data)]
    if STATUS_BASE < log_id <= STATUS_BASE + NODE_COUNT or \
            DIAGNOSTICS_BASE < log_id <= DIAGNOSTICS_BASE + NODE_COUNT:
        return [(log_id, data[:HEALTH_PAYLOAD])]
    if EVENT_BASE < log_id <= EVENT_BASE + NODE_COUNT:
        return [(log_id, data)]
    if log_id == AIM_RECORD:
        rpm = max(0, min(0xFFFF, payload & 0xFFFFFFFF))
        return [(AIM_WIRE, struct.pack('<HH', rpm, counter & 0xFFFF))]
    return []   # the master's own health (node 0), GPS, anything else: never on the bus


def read_records(path):
    """(id, payload) of every complete 16-byte record; a trailing partial one is ignored."""
    data = Path(path).read_bytes()
    usable = len(data) - len(data) % 16
    return list(struct.iter_unpack('<QQ', data[:usable]))


def sample(can_id, value, t_ms):
    return can_id, struct.pack('<iI', int(value), t_ms & 0xFFFFFFFF)


class Synthetic:
    """Plausible values on a 20 s cycle: accelerate 12 s, brake 3 s, coast 5 s."""

    PERIOD_MS = {0x0B1: 10, 0x0B2: 10, 0x0B3: 10, 0x0B4: 10, 0x0B5: 10, 0x0B6: 10, 0x0B7: 10,
                 0x0B8: 10, 0x0B9: 20, 0x0BA: 10, 0x0BD: 10, 0x0BE: 10, 0x0BF: 10, 0x0C9: 10}
    ENGINE_START_MS = 3000

    def __init__(self, nodes, seed=1):
        self.nodes = set(nodes)
        self.rng = random.Random(seed)
        self.next_due = {can_id: 0 for can_id in self.PERIOD_MS if SENSOR_NODE[can_id] in self.nodes}
        self.next_zero_ms = 0
        self.next_spark_ms = self.ENGINE_START_MS

    @staticmethod
    def phase(t_ms):
        """(throttle 0..1, braking 0..1) at this time."""
        c = (t_ms / 1000.0) % 20.0
        if c < 12.0:
            return c / 12.0, 0.0
        if c < 15.0:
            return 0.0, math.sin(math.pi * (c - 12.0) / 3.0)
        return 0.2, 0.0

    def rpm(self, t_ms):
        throttle, _ = self.phase(t_ms)
        return 1800 + 1800 * throttle

    def value(self, can_id, t_ms):
        throttle, braking = self.phase(t_ms)
        noise = self.rng.uniform(-1.0, 1.0)
        wave = math.sin(2 * math.pi * t_ms / 7000.0)
        if can_id == 0x0B1:
            return max(0, min(1500, 1200 * braking + 5 * noise))
        if can_id == 0x0B2:
            return max(0, min(1500, 900 * braking + 5 * noise))
        if can_id == 0x0B3:
            return max(-1500, min(1500, 1500 * (throttle - braking) - 300 + 20 * noise))
        if can_id == 0x0B4:
            return 800 * wave + 20 * noise
        if can_id == 0x0B5:
            return 1000 + 50 * noise
        if can_id in (0x0B6, 0x0B7, 0x0B8):
            return 50000 * wave * (0.2 + 0.4 * (can_id - 0x0B6))
        if can_id == 0x0B9:
            if t_ms < self.ENGINE_START_MS or braking > 0.3:
                return 0
            return self.rpm(t_ms) * 0.6 * (0.3 + 0.7 * throttle)
        if can_id == 0x0BA:
            return 2000 + 1000 * wave
        if can_id in (0x0BD, 0x0BE, 0x0BF, 0x0C9):
            return max(20, min(130, 75 + 40 * wave * (1 if can_id in (0x0BD, 0x0BF) else -1) + 3 * noise))
        return 0

    def samples_due(self, t_ms):
        """Every sample frame due at t_ms, stamped t_ms."""
        frames = []
        for can_id, due in self.next_due.items():
            if t_ms >= due:
                frames.append(sample(can_id, self.value(can_id, t_ms), t_ms))
                period = self.PERIOD_MS[can_id]
                self.next_due[can_id] = due + period if t_ms - due < 10 * period else t_ms + period
        if 5 in self.nodes:
            if t_ms < self.ENGINE_START_MS:
                if t_ms >= self.next_zero_ms:   # stopped engine: zeros every 100 ms
                    frames.append(sample(0x0BB, 0, t_ms))
                    self.next_zero_ms = t_ms + 100
            else:
                while t_ms >= self.next_spark_ms:   # running: one spark and one RPM per revolution
                    rpm = self.rpm(t_ms)
                    frames.append(sample(0x0BC, 1, t_ms))
                    frames.append(sample(0x0BB, rpm, t_ms))
                    self.next_spark_ms += 60000.0 / rpm
        return frames


class Bus:
    def __init__(self, iface, stop_node=0, stop_after_s=None):
        self.sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self.sock.bind((iface,))
        self.started = time.monotonic()
        self.stop_node = stop_node
        self.stop_after_s = stop_after_s

    def silenced(self, can_id):
        return (self.stop_node and self.stop_after_s is not None and node_of(can_id) == self.stop_node and
                time.monotonic() - self.started >= self.stop_after_s)

    def send(self, can_id, data):
        if self.silenced(can_id):
            return
        frame = CAN_FRAME.pack(can_id, len(data), bytes(data).ljust(8, b'\0'))
        for _ in range(100):
            try:
                self.sock.send(frame)
                return
            except OSError:   # transmit queue full: give the reader a moment
                time.sleep(0.001)

    def beacon(self, master_ms, recording):
        value = (master_ms * 1000) | (RECORDING_FLAG if recording else 0)
        self.send(BEACON, value.to_bytes(8, 'little'))

    def state(self, node, recording, reason):
        self.send(STATE_BASE + node, bytes([1 if recording else 0, reason, 0]))


def run_synthetic(bus, nodes, master):
    sim = Synthetic(nodes)
    for node in sorted(nodes):
        bus.state(node, False, REASON_BOOT)
    start = time.monotonic()
    next_beacon = next_health = 0
    while True:
        t_ms = int((time.monotonic() - start) * 1000)
        for can_id, data in sim.samples_due(t_ms):
            bus.send(can_id, data)
        if master and t_ms >= next_beacon:
            bus.beacon(t_ms, recording=False)
            next_beacon += 100
        if t_ms >= next_health:
            for node in sorted(nodes):
                status = 0   # no faults, health OK, mode idle, every sensor OK
                bus.send(STATUS_BASE + node, status.to_bytes(4, 'little'))
                bus.send(DIAGNOSTICS_BASE + node, bytes([0, 0, 0, 1]))
            next_health += 1000
        time.sleep(0.001)


def run_replay(bus, records, speed, loop, master):
    timestamps = [payload >> 32 for _, payload in records]
    if not records:
        sys.exit('daq_sim: the recording has no complete record')
    nodes = sorted({node_of(log_id) for log_id, _ in records} - {0})
    while True:
        for node in nodes:
            bus.state(node, True, REASON_START)
        t0 = timestamps[0]
        start = time.monotonic()
        next_beacon = t0
        aim_counter = 0
        for (log_id, payload), ts in zip(records, timestamps):
            due = start + (ts - t0) / 1000.0 / speed
            delay = due - time.monotonic()
            if delay > 0:   # a timestamp that goes backwards is sent right away
                time.sleep(delay)
            while master and ts >= next_beacon:
                bus.beacon(next_beacon, recording=True)
                next_beacon += 100
            for can_id, data in record_to_frames(log_id, payload, aim_counter):
                bus.send(can_id, data)
            if log_id == AIM_RECORD:
                aim_counter += 1
        if not loop:
            return


def main(argv=None):
    parser = argparse.ArgumentParser(description='Send DAQ traffic on a virtual CAN bus.')
    parser.add_argument('iface', help='interface, for example vcan0')
    parser.add_argument('--replay', type=Path, help='master .bin recording to replay')
    parser.add_argument('--speed', type=float, default=1.0, help='replay speed factor (default 1)')
    parser.add_argument('--loop', action='store_true', help='restart the replay at the end')
    parser.add_argument('--nodes', default='1,3,4,5,7,8', help='synthetic nodes (default 1,3,4,5,7,8)')
    parser.add_argument('--no-master', action='store_true', help='send no beacon, as if the master were off')
    parser.add_argument('--stop-node', type=int, default=0, help='silence this node ...')
    parser.add_argument('--after', type=float, help='... after this many seconds')
    parser.add_argument('--allow-real-interface', action='store_true',
                        help='allow an interface not named vcan*; never on the car')
    args = parser.parse_args(argv)

    if not args.iface.startswith('vcan') and not args.allow_real_interface:
        print(f'daq_sim: refusing to transmit on {args.iface}: only vcan* interfaces are allowed, '
              'so fake frames never reach the car. Use --allow-real-interface on a bench bus.',
              file=sys.stderr)
        return 2
    if args.speed <= 0:
        parser.error('--speed must be positive')
    nodes = {int(n) for n in args.nodes.split(',') if n}
    if not nodes <= set(range(1, NODE_COUNT + 1)):
        parser.error('--nodes takes node IDs 1 to 8')

    bus = Bus(args.iface, args.stop_node, args.after)
    try:
        if args.replay:
            run_replay(bus, read_records(args.replay), args.speed, args.loop, not args.no_master)
        else:
            run_synthetic(bus, nodes, not args.no_master)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
