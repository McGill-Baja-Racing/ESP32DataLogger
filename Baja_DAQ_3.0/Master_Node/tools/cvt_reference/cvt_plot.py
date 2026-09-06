#!/usr/bin/env python3
"""
Baja CVT shift curve + driven-clutch torque, from the CAN log CSV.

    pip install numpy pandas scipy matplotlib

Set LOG_PATH in the CONFIG block below and just run the file, or pass a path
on the command line to override it:

    python cvt_plot.py                           # uses LOG_PATH
    python cvt_plot.py log.csv --idler-ratio 2.75
    python cvt_plot.py log.csv --calibrate       # guess the gearbox ratio
    python cvt_plot.py log.csv --segments        # list runs, then pick one
    python cvt_plot.py log.csv --segment 2

Input: one row per CAN sample, channels interleaved and split by can_id.

    sample_index,can_id,can_id_hex,signal,node,timestamp_ms,value,units,raw_data
    0,187,0x0BB,engine_rpm,engine_node_5,300095,1680,rpm,...

Only can_id, timestamp_ms and value are used. raw_data is just
(timestamp_ms << 32) | value, so it carries nothing new.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy.signal import savgol_filter

# ==========================================================================
# CONFIG
# ==========================================================================
# The log to analyse. Relative paths resolve against this script's folder, so
# dropping the CSV next to cvt_plot.py and putting its name here is enough.
# A path given on the command line overrides this.
LOG_PATH  = "mock_log.csv"
OUT_PNG   = "cvt_plot.png"      # figure
OUT_CSV   = None                # e.g. "results.csv" for the per-sample frame

ENGINE_CAN_ID = 187             # 0x0BB
IDLER_CAN_ID  = 185             # 0x0B9

# >>> UNKNOWN: N_driven_clutch / N_idler. Count the teeth.
#     --calibrate gives a stopgap estimate. Every ratio and torque scales with it.
IDLER_TO_DRIVEN = 1.69565

# Plausibility gate. Period-measurement glitches (a missed or extra edge) throw
# wildly wrong instantaneous values, sometimes in runs long enough to survive a
# median filter, so they get dropped outright rather than smoothed.
#
# ENGINE_RPM_MAX is a physical limit: set it just above where the governor holds.
# The idler ceiling is derived from it, since the idler can't turn faster than
# the engine does through the tallest ratio the CVT can reach.
ENGINE_RPM_MAX  = 4000.0        # restricted CH440; governed near 3800
RPM_OFF         = 10.0          # at or below this the shaft is stopped

CVT_LOW         = 3.01          # full underdrive
CVT_HIGH        = 0.44          # full overdrive
CVT_EFFICIENCY  = 0.85
DRIVEN_MIN_RPM  = 50.0          # below this the car isn't moving; ratio undefined

GRID_HZ         = 50.0          # common time base (matches the 20 ms logging)
MEDIAN_WIN_S    = 0.10          # rolling median, kills the ±10% period jitter
SMOOTH_WIN_S    = 0.40          # Savitzky-Golay window
SMOOTH_ORDER    = 2
MAX_GAP_S       = 0.20          # don't interpolate across anything longer
SEGMENT_GAP_S   = 2.0           # a gap this long starts a new run

# SAE Baja restricted Kohler CH440, net corrected
DYNO_RPM    = np.array([2400, 2600, 2800, 3000, 3200, 3400, 3600], float)
DYNO_TORQUE = np.array([18.5, 18.1, 17.4, 16.6, 15.4, 14.5, 13.5], float)

MIN_PLOT_RPM = 1500.0           # ignore idle in the shift curve
# ==========================================================================


# --------------------------------------------------------------------------
# Load and split by can_id
# --------------------------------------------------------------------------
def resolve(path):
    """Absolute if given, otherwise relative to this script's folder."""
    p = Path(path).expanduser()
    return p if p.is_absolute() else (Path(__file__).resolve().parent / p)


def idler_rpm_max(idler_to_driven):
    """Fastest the idler can physically turn: engine redline through the tallest
    CVT ratio, then back through the gearbox. Margin for measurement scatter."""
    return 1.15 * ENGINE_RPM_MAX / CVT_HIGH / max(idler_to_driven, 1e-6)


def load(path, idler_to_driven=1.0):
    """Return {'engine': (t_s, rpm), 'idler': (t_s, rpm)}, sorted and cleaned."""
    df = pd.read_csv(path, usecols=lambda c: c in
                     {"can_id", "timestamp_ms", "value", "signal"})
    present = sorted(int(c) for c in df["can_id"].unique())
    out = {}
    for key, cid, cap in (("engine", ENGINE_CAN_ID, ENGINE_RPM_MAX),
                          ("idler", IDLER_CAN_ID, idler_rpm_max(idler_to_driven))):
        g = df[df["can_id"] == cid]
        if g.empty:
            out[key] = (np.array([]), np.array([]))
            continue
        t = g["timestamp_ms"].to_numpy(float) / 1000.0
        v = g["value"].to_numpy(float)
        o = np.argsort(t, kind="stable")
        t, v = t[o], v[o]
        keep = np.concatenate(([True], np.diff(t) > 0))      # drop dup stamps
        t, v = t[keep], v[keep]
        bad = (v > cap) | ~np.isfinite(v)                    # implausible -> gone
        n_bad = int(bad.sum())
        t, v = t[~bad], v[~bad]
        print(f"  can_id {cid:>3} ({key:6s}): {len(g):6d} rows, "
              f"{n_bad:5d} dropped above {cap:.0f} rpm ({100*n_bad/len(g):.1f}%), "
              f"{int((v > RPM_OFF).sum()):6d} running")
        out[key] = (t, v)
    out["_present"] = present
    return out


def segments(t, gap_s=SEGMENT_GAP_S):
    """Split a time vector into continuous runs at long logging gaps."""
    if t.size == 0:
        return []
    brk = np.flatnonzero(np.diff(t) > gap_s)
    starts = np.concatenate(([0], brk + 1))
    stops = np.concatenate((brk + 1, [t.size]))
    return [(int(a), int(b)) for a, b in zip(starts, stops)]


# --------------------------------------------------------------------------
# Common time base + smoothing
# --------------------------------------------------------------------------
def _odd(n, lo=3):
    n = int(round(n))
    return max(n + (n % 2 == 0), lo)


def resample(t, v, grid):
    """Linear interpolation onto the grid, blanking anything past a real gap."""
    if t.size < 2:
        return np.full(grid.size, np.nan)
    y = np.interp(grid, t, v, left=np.nan, right=np.nan)
    for i in np.flatnonzero(np.diff(t) > MAX_GAP_S):
        y[(grid > t[i]) & (grid < t[i + 1])] = np.nan
    y[(grid < t[0]) | (grid > t[-1])] = np.nan
    return y


def smooth(x, fs):
    """Rolling median then Savitzky-Golay, zero-phase, NaNs preserved."""
    x = np.asarray(x, float)
    bad = ~np.isfinite(x)
    if bad.all():
        return x
    y = pd.Series(x).interpolate(limit_direction="both").to_numpy()
    k = _odd(MEDIAN_WIN_S * fs)
    if k >= 3:
        y = pd.Series(y).rolling(k, center=True, min_periods=1).median().to_numpy()
    w = min(_odd(SMOOTH_WIN_S * fs, SMOOTH_ORDER + 2), _odd(y.size - 1))
    if w > SMOOTH_ORDER:
        y = savgol_filter(y, w, SMOOTH_ORDER, mode="interp")
    return np.where(bad, np.nan, y)


# --------------------------------------------------------------------------
# Engine map
# --------------------------------------------------------------------------
def engine_torque(rpm):
    """WOT net-corrected torque, held flat outside the mapped 2400-3600 range."""
    return np.interp(np.asarray(rpm, float), DYNO_RPM, DYNO_TORQUE)


def engine_power_hp(rpm):
    return engine_torque(rpm) * np.asarray(rpm, float) / 5252.0


PEAK_POWER_RPM = float(np.linspace(2400, 3600, 4001)[
    np.argmax(engine_power_hp(np.linspace(2400, 3600, 4001)))])


def power_band(frac=0.98):
    g = np.linspace(DYNO_RPM[0], DYNO_RPM[-1], 4001)
    p = engine_power_hp(g)
    ok = p >= frac * p.max()
    return float(g[ok][0]), float(g[ok][-1])


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------
def analyse(chans, idler_to_driven, seg=None):
    te, ve = chans["engine"]
    ti, vi = chans["idler"]
    have_idler = ti.size > 1

    lo = te[0] if not have_idler else max(te[0], ti[0])
    hi = te[-1] if not have_idler else min(te[-1], ti[-1])
    if seg is not None:
        lo, hi = max(lo, seg[0]), min(hi, seg[1])
    if hi <= lo:
        raise ValueError("Engine and idler channels don't overlap in time.")

    grid = np.arange(lo, hi, 1.0 / GRID_HZ)
    eng = smooth(resample(te, ve, grid), GRID_HZ)
    idl = smooth(resample(ti, vi, grid), GRID_HZ) if have_idler \
        else np.full(grid.size, np.nan)

    driven = idl * idler_to_driven
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = eng / driven
    ratio = np.where(np.isfinite(ratio) & (driven >= DRIVEN_MIN_RPM), ratio, np.nan)

    # Outside the mechanical range = belt slip, wrong gearbox ratio, or a bad
    # sensor. Dropped so it can't masquerade as a real CVT state.
    tol = 0.05
    in_range = np.isfinite(ratio) & (ratio <= CVT_LOW * (1 + tol)) \
                                  & (ratio >= CVT_HIGH * (1 - tol))
    n_out = int((np.isfinite(ratio) & ~in_range).sum())
    ratio = np.where(in_range, ratio, np.nan)

    tq_eng = engine_torque(eng)
    tq_driven = tq_eng * ratio * CVT_EFFICIENCY

    df = pd.DataFrame({
        "t_s": grid,
        "t_rel_s": grid - grid[0],
        "engine_rpm": eng,
        "idler_rpm": idl,
        "driven_rpm": driven,
        "cvt_ratio": ratio,
        "engine_torque_ftlb": tq_eng,
        "engine_power_hp": engine_power_hp(eng),
        "engine_rpm_outside_dyno_map": (eng < DYNO_RPM[0]) | (eng > DYNO_RPM[-1]),
        "driven_torque_ftlb": tq_driven,
        "driven_power_hp": tq_driven * driven / 5252.0,
    })
    return df, n_out, have_idler


def calibrate(chans, pct=1.0):
    """Stopgap N_driven/N_idler: assume the CVT is fully shifted out at the
    fastest point of the run, so the true ratio there is CVT_HIGH."""
    df, _, have = analyse(chans, 1.0)
    if not have:
        return float("nan")
    e, w = df["engine_rpm"].to_numpy(), df["idler_rpm"].to_numpy()
    ok = np.isfinite(e) & np.isfinite(w) & (w > DRIVEN_MIN_RPM) & (e > MIN_PLOT_RPM)
    if not ok.any():
        return float("nan")
    return float(np.percentile(e[ok] / w[ok], pct) / CVT_HIGH)


# --------------------------------------------------------------------------
# Plot
# --------------------------------------------------------------------------
def make_figure(df, k, have_idler, title=""):
    plt.rcParams.update({"figure.dpi": 110, "savefig.dpi": 150, "font.size": 9,
                         "axes.grid": True, "grid.alpha": 0.3,
                         "axes.axisbelow": True, "figure.facecolor": "white"})
    fig, ax = plt.subplots(2, 2, figsize=(13, 8.5), constrained_layout=True)
    lo, hi = power_band()
    d = df.dropna(subset=["cvt_ratio"])
    d = d[d["engine_rpm"] > MIN_PLOT_RPM]

    # ---- shift curve ----
    a = ax[0, 0]
    if len(d):
        sc = a.scatter(d["cvt_ratio"], d["engine_rpm"], c=d["t_rel_s"], s=5,
                       cmap="viridis", alpha=0.55, linewidths=0)
        fig.colorbar(sc, ax=a, label="time [s]", pad=0.01)
        bins = np.linspace(d["cvt_ratio"].min(), d["cvt_ratio"].max(), 41)
        g = d.groupby(pd.cut(d["cvt_ratio"], bins), observed=True)
        m = g.agg(r=("cvt_ratio", "median"), q=("engine_rpm", "median"),
                  p10=("engine_rpm", lambda x: x.quantile(.10)),
                  p90=("engine_rpm", lambda x: x.quantile(.90)),
                  n=("engine_rpm", "size")).reset_index(drop=True)
        m = m[m["n"] >= 3].dropna()
        a.plot(m["r"], m["q"], lw=2.2, color="#c0392b", label="median")
        a.fill_between(m["r"], m["p10"], m["p90"], color="#c0392b", alpha=.18,
                       label="10-90th pct")
    else:
        a.text(.5, .5, "no idler data\nno ratio, no shift curve", ha="center",
               va="center", transform=a.transAxes, fontsize=13, color="#c0392b")
    a.axhline(PEAK_POWER_RPM, color="k", ls="--", lw=1,
              label=f"peak power {PEAK_POWER_RPM:.0f} rpm")
    a.axhspan(lo, hi, color="k", alpha=.07, label="98% power band")
    for r in (CVT_LOW, CVT_HIGH):
        a.axvline(r, color="0.4", ls=":", lw=1)
    a.set_xlim(CVT_LOW * 1.08, CVT_HIGH * .9)
    a.set_xlabel("CVT ratio  (N_engine / N_driven)    low ⟵    ⟶ high")
    a.set_ylabel("engine speed [rpm]")
    a.set_title("CVT shift curve")
    a.legend(fontsize=8, loc="lower right")

    # ---- driven clutch torque vs ratio ----
    a = ax[0, 1]
    dd = df.dropna(subset=["cvt_ratio", "driven_torque_ftlb"])
    if len(dd):
        sc = a.scatter(dd["cvt_ratio"], dd["driven_torque_ftlb"], c=dd["t_rel_s"],
                       s=5, cmap="viridis", alpha=.55, linewidths=0)
        fig.colorbar(sc, ax=a, label="time [s]", pad=0.01)
    sweep = np.linspace(CVT_HIGH, CVT_LOW, 200)
    a.plot(sweep, engine_torque(PEAK_POWER_RPM) * sweep * CVT_EFFICIENCY,
           color="#e67e22", lw=2, ls="--",
           label=f"ideal: engine held at {PEAK_POWER_RPM:.0f} rpm")
    a.set_xlim(CVT_LOW * 1.08, CVT_HIGH * .9)
    a.set_xlabel("CVT ratio")
    a.set_ylabel("driven clutch torque [ft-lb]")
    a.set_title(f"Driven clutch torque  (T_eng × ratio × {CVT_EFFICIENCY})")
    a.legend(fontsize=8)

    # ---- torque vs time ----
    a = ax[1, 0]
    run = df["engine_rpm"] > RPM_OFF
    a.plot(df["t_rel_s"].where(run), df["engine_torque_ftlb"].where(run),
           color="#c0392b", lw=1.1, label="engine (WOT map)")
    ex = df["engine_rpm_outside_dyno_map"].to_numpy() & run.to_numpy()
    if ex.any():
        a.fill_between(df["t_rel_s"], 0, df["engine_torque_ftlb"], where=ex,
                       color="0.6", alpha=.3, label="rpm outside dyno map")
    a.plot(df["t_rel_s"], df["driven_torque_ftlb"], color="#2c3e50", lw=1.3,
           label="driven clutch")
    a.set_xlabel("time [s]")
    a.set_ylabel("torque [ft-lb]")
    a.set_title("Torque available through the CVT")
    a.legend(fontsize=8)

    # ---- speeds + ratio vs time ----
    a = ax[1, 1]
    a.plot(df["t_rel_s"], df["engine_rpm"], color="#c0392b", lw=1.0, label="engine")
    if have_idler:
        a.plot(df["t_rel_s"], df["driven_rpm"], color="#16a085", lw=1.0,
               label="driven clutch")
    a.set_xlabel("time [s]")
    a.set_ylabel("speed [rpm]")
    a.legend(fontsize=8, loc="upper left")
    if have_idler:
        b = a.twinx()
        b.plot(df["t_rel_s"], df["cvt_ratio"], color="#2c3e50", lw=1.2)
        b.set_ylabel("CVT ratio", color="#2c3e50")
        b.set_ylim(CVT_HIGH * .85, CVT_LOW * 1.15)
        b.invert_yaxis()
        b.grid(False)
    a.set_title("Speeds and ratio vs time")

    note = f"N_driven/N_idler = {k:.4f}" + ("  (PLACEHOLDER)" if abs(k - 1) < 1e-9 else "")
    if not have_idler:
        note = f"can_id {IDLER_CAN_ID} not in file — engine channel only"
    fig.suptitle(f"{title}  |  {note}  |  engine torque is a WOT upper bound",
                 fontsize=10)
    return fig


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default=None,
                    help=f"CAN log CSV (default: LOG_PATH = {LOG_PATH!r})")
    ap.add_argument("--idler-ratio", type=float, help="N_driven / N_idler")
    ap.add_argument("--calibrate", action="store_true",
                    help="estimate N_driven/N_idler from this log")
    ap.add_argument("--segments", action="store_true", help="list runs and exit")
    ap.add_argument("--segment", type=int, help="analyse only this run (1-based)")
    ap.add_argument("--max-rpm", type=float,
                    help="engine plausibility ceiling in rpm")
    ap.add_argument("--out", default=None, help="output PNG")
    ap.add_argument("--csv", default=None,
                    help="also write the per-sample results here")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args(argv)

    global ENGINE_RPM_MAX
    if args.max_rpm:
        ENGINE_RPM_MAX = args.max_rpm
    k = args.idler_ratio if args.idler_ratio else IDLER_TO_DRIVEN

    log = resolve(args.log or LOG_PATH)
    if not log.exists():
        print(f"! {log} not found. Set LOG_PATH in the CONFIG block, or pass a "
              "path on the command line. Run make_mock_log.py to generate a "
              "test file in the right format.")
        return 1

    print(f"reading {log}")
    chans = load(log, k)
    if chans["engine"][0].size == 0:
        print(f"! no can_id {ENGINE_CAN_ID} rows found; "
              f"file contains {chans['_present']}")
        return 1
    have_idler = chans["idler"][0].size > 1
    if not have_idler:
        print(f"! no can_id {IDLER_CAN_ID} (idler) rows in this file — "
              f"it only contains {chans['_present']}. Engine-side plots only; "
              "the shift curve and driven torque need both nodes logging.")

    # ---- segments ----
    te = chans["engine"][0]
    segs = segments(te)
    print(f"\n{len(segs)} continuous run(s) (split at gaps > {SEGMENT_GAP_S:g} s):")
    for n, (a, b) in enumerate(segs, 1):
        run = chans["engine"][1][a:b] > RPM_OFF
        print(f"  [{n}] t = {te[a]:8.1f} - {te[b-1]:8.1f} s "
              f"({te[b-1]-te[a]:6.1f} s, {b-a:6d} samples, "
              f"engine running {100*run.mean():5.1f}%)")
    if args.segments:
        return 0

    window = None
    if args.segment:
        a, b = segs[args.segment - 1]
        window = (te[a], te[b - 1])
        print(f"\nusing run {args.segment}: {window[0]:.1f} - {window[1]:.1f} s")

    if args.calibrate:
        k = calibrate(chans)
        print(f"\n[calibrate] N_driven/N_idler ~ {k:.4f}  "
              f"(assumes the CVT reaches {CVT_HIGH:.2f} at max speed)")

    df, n_out, have_idler = analyse(chans, k, window)

    # ---- report ----
    run = df[df["engine_rpm"] > RPM_OFF]
    print(f"\nengine running {len(run)/GRID_HZ:.1f} s of {len(df)/GRID_HZ:.1f} s")
    if len(run):
        print(f"engine rpm     median {run['engine_rpm'].median():.0f}, "
              f"p95 {run['engine_rpm'].quantile(.95):.0f}   "
              f"(peak power at {PEAK_POWER_RPM:.0f})")
    d = df.dropna(subset=["cvt_ratio"])
    if len(d):
        lo, hi = power_band()
        print(f"ratio spanned  {d['cvt_ratio'].min():.2f} -> {d['cvt_ratio'].max():.2f}"
              f"   (CVT range {CVT_HIGH}-{CVT_LOW})")
        print(f"in 98% band    {d['engine_rpm'].between(lo, hi).mean()*100:.1f}% "
              "of engaged time")
        print(f"driven torque  max {d['driven_torque_ftlb'].max():.1f}, "
              f"median {d['driven_torque_ftlb'].median():.1f} ft-lb")
    if n_out:
        print(f"! {n_out} samples outside the CVT ratio range — belt slip, "
              "wrong gearbox ratio, or a sensor fault")
    if have_idler and abs(k - 1.0) < 1e-9:
        print("! N_driven/N_idler is still the 1.0 placeholder; "
              "all ratios and torques scale with it")

    fig = make_figure(df, k, have_idler, log.name)
    out_png = resolve(args.out or OUT_PNG)
    fig.savefig(out_png, bbox_inches="tight")
    print(f"\nwrote {out_png}")
    out_csv = args.csv or OUT_CSV
    if out_csv:
        df.to_csv(resolve(out_csv), index=False)
        print(f"wrote {resolve(out_csv)}")
    if args.show:
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
