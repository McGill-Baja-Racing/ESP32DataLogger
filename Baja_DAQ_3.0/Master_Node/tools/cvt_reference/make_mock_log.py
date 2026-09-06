#!/usr/bin/env python3
"""
Generate a mock CAN log in the same format as the real DAQ export, with BOTH
sensors present, so cvt_plot.py can be tested before the idler node is logging.

    python make_mock_log.py                  # writes mock_log.csv
    python make_mock_log.py --out test.csv --runs 4 --seed 12

Schema is copied from the sample export:

    sample_index,can_id,can_id_hex,signal,node,timestamp_ms,value,units,raw_data

Rows from both nodes are interleaved and sorted by timestamp, exactly as they
come off the SD card. raw_data is reproduced as (timestamp_ms << 32) | value,
which is what the sample file contained.

What gets baked in on purpose, because the real log had all of it:
  * 50 Hz logging (20 ms), with occasional 19/21 ms jitter
  * long gaps between runs (logger stopped and restarted)
  * exact zeros when a shaft is stopped
  * period-measurement glitches in RUNS of several samples, not lone spikes,
    with a 20000 clamp value -- these are what the plausibility gate is for
  * ±10% sample-to-sample scatter from period quantisation

NOTE: the sample export only contained can_id 187, so the signal/node strings
for 185 below are a guess. Change IDLER_SIGNAL / IDLER_NODE to match whatever
the real DBC emits. cvt_plot.py never reads those columns, so it makes no
difference to the analysis either way.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

# ---- schema ----
ENGINE_CAN_ID, ENGINE_HEX = 187, "0x0BB"
IDLER_CAN_ID,  IDLER_HEX  = 185, "0x0B9"
ENGINE_SIGNAL, ENGINE_NODE = "engine_rpm", "engine_node_5"
IDLER_SIGNAL,  IDLER_NODE  = "idler_rpm", "gearbox_node_3"   # <- guess

# ---- what the mock car does ----
RATE_HZ          = 50.0
CLOCK_START_MS   = 300_095      # logger uptime when recording began
IDLER_TO_DRIVEN  = 2.75         # truth value; --calibrate should recover this
CVT_LOW, CVT_HIGH = 3.01, 0.44
IDLE_RPM         = 1800.0
SHIFT_RPM        = 3050.0       # engine speed the clutches are tuned to hold

GLITCH_RATE      = 0.010        # glitch runs per sample
GLITCH_LEN       = (2, 20)      # samples per run
QUANT_NOISE_PCT  = 0.05         # period-measurement scatter
DROPOUT_RATE     = 0.0004       # short lost-packet gaps


def _profile(t, rng):
    """One acceleration run: idle, launch, shift out, hill backshift, lift."""
    T = t[-1]
    ramp = lambda x, a, b: np.clip((x - a) / (b - a), 0.0, 1.0)

    launch = rng.uniform(1.5, 3.0)
    eng = IDLE_RPM + (SHIFT_RPM - IDLE_RPM) * ramp(t, launch, launch + 0.9)

    # shift out to overdrive over a few seconds
    shift_end = launch + rng.uniform(6.0, 9.0)
    ratio = CVT_LOW - (CVT_LOW - 0.95) * ramp(t, launch + 1.0, shift_end) ** 1.35

    # a hill partway through: CVT backshifts, engine gets pulled down
    h0 = shift_end + rng.uniform(1.0, 3.0)
    hill = ramp(t, h0, h0 + 1.2) * (1 - ramp(t, h0 + 4.0, h0 + 5.5))
    ratio = ratio + rng.uniform(1.2, 1.9) * hill
    eng = eng - 250 * hill

    # lift off at the end
    lift = ramp(t, T - 3.0, T - 1.5)
    eng = eng * (1 - 0.5 * lift) + IDLE_RPM * 0.5 * lift
    ratio = ratio + (CVT_LOW - ratio) * lift

    ratio = np.clip(ratio, CVT_HIGH, CVT_LOW)
    eng = eng + 40 * np.sin(2 * np.pi * 1.7 * t)        # firing/load wobble

    idler = eng / ratio / IDLER_TO_DRIVEN
    idler[t < launch + 0.6] = 0.0                        # stationary before launch
    eng[t < launch - 1.2] = 0.0                          # engine not yet cranked
    return eng, idler


def _corrupt(v, rng):
    """Add the measurement pathologies the real sensor showed."""
    v = v.astype(float).copy()
    running = v > 0

    # period quantisation: ±few % scatter, only while the shaft turns
    v[running] *= 1 + rng.normal(0, QUANT_NOISE_PCT, running.sum())

    # glitch RUNS -- missed/extra edges hold a wrong value for several samples
    n = v.size
    for start in np.flatnonzero(rng.random(n) < GLITCH_RATE):
        if not running[start]:
            continue
        length = rng.integers(*GLITCH_LEN)
        end = min(start + length, n)
        if rng.random() < 0.25:
            v[start:end] = 20000                          # sensor clamp
        else:
            v[start:end] = rng.uniform(5000, 19999, end - start)
    return np.maximum(v, 0).round()


def _timestamps(n, t0_ms, rng):
    """20 ms nominal with the odd 19/21 ms, plus short lost-packet gaps."""
    dt = np.full(n, 20)
    dt[rng.random(n) < 0.002] = 19
    dt[rng.random(n) < 0.0005] = 21
    drops = rng.random(n) < DROPOUT_RATE
    dt[drops] += rng.integers(200, 700, drops.sum())      # 0.2-0.7 s gaps
    return t0_ms + np.cumsum(dt) - dt[0]


def make_log(n_runs=3, seed=7):
    rng = np.random.default_rng(seed)
    frames = []
    clock = CLOCK_START_MS

    for r in range(n_runs):
        dur = rng.uniform(18.0, 30.0)
        n = int(dur * RATE_HZ)
        t = np.arange(n) / RATE_HZ
        eng, idler = _profile(t, rng)

        for cid, hexid, sig, node, vals in (
                (ENGINE_CAN_ID, ENGINE_HEX, ENGINE_SIGNAL, ENGINE_NODE, eng),
                (IDLER_CAN_ID, IDLER_HEX, IDLER_SIGNAL, IDLER_NODE, idler)):
            # Each node timestamps its own samples, so the two are close but
            # not identical -- a few ms of offset between the boards.
            ts = _timestamps(n, clock + rng.integers(0, 6), rng)
            v = _corrupt(vals, rng).astype(np.int64)
            frames.append(pd.DataFrame({
                "can_id": cid, "can_id_hex": hexid, "signal": sig, "node": node,
                "timestamp_ms": ts, "value": v, "units": "rpm",
            }))

        # logger stopped between runs
        clock += int(dur * 1000) + int(rng.uniform(5, 120) * 1000)

    df = pd.concat(frames).sort_values("timestamp_ms", kind="stable")
    df = df.reset_index(drop=True)
    df.insert(0, "sample_index", np.arange(len(df)))
    ts = df["timestamp_ms"].to_numpy(np.int64)
    val = df["value"].to_numpy(np.int64)
    df["raw_data"] = (ts << 32) | val
    return df[["sample_index", "can_id", "can_id_hex", "signal", "node",
               "timestamp_ms", "value", "units", "raw_data"]]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="mock_log.csv")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    df = make_log(args.runs, args.seed)
    out = Path(args.out)
    if not out.is_absolute():
        out = Path(__file__).resolve().parent / out
    df.to_csv(out, index=False)

    print(f"wrote {out}  ({len(df)} rows, {args.runs} runs)")
    for cid, g in df.groupby("can_id"):
        v = g["value"].to_numpy()
        print(f"  can_id {cid}: {len(g):6d} rows, "
              f"{(v == 0).sum():5d} zero, "
              f"{(v > 4000).sum():5d} glitched, "
              f"median running {np.median(v[(v > 10) & (v < 4000)]):.0f} rpm")
    print(f"\ntruth: N_driven/N_idler = {IDLER_TO_DRIVEN}")
    print("run:   python cvt_plot.py  (with LOG_PATH pointing here)")


if __name__ == "__main__":
    main()
