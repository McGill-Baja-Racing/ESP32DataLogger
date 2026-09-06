# Offline CVT analysis

On either engine-rpm-prototype or wheel-events-prototype, flash the updated
MasterStable firmware. Node firmware and CAN recording formats do not change.
After Stop completes, select **CVT analysis** beside BIN/CSV for a completed log.
The page downloads that log and automatically analyses it on the phone/laptop.
No internet, Python service, account, or external upload is needed. Browser
assets, including Chart.js 4.5.1 and its license, are embedded in the firmware.
The analysis uses a Web Worker so processing does not block page interaction.

Outputs:

- Four charts: CVT shift curve with median/10–90 percentiles and the 98% power
  band, estimated driven torque versus ratio, estimated torque versus time,
  and shaft speeds/CVT ratio versus time.
- Engine running time, median/p95 RPM, ratio span, time in the power band,
  estimated driven torque and rejected-sample counts.
- Figure PNG, complete processed CSV, and a text summary with settings.
- All runs or one run, an editable RPM ceiling and shaft ratio, and an optional
  ratio estimate. Local BIN/CSV files can also be opened on the same page.

## Inputs and assumptions

The browser port follows the supplied `tools/cvt_reference/cvt_plot.py`.
The original analysis and mock generator are preserved unchanged for desktop
use and numerical comparisons. Their four Python dependencies are listed in
`tools/cvt_reference/requirements.txt`.

Engine input is CAN 0x0BB (node 5); idler input is 0x0B9 (node 4). Extra spark,
wheel-edge and master-paired records are ignored. Driven RPM = idler RPM ×
**1.69565**, the user-confirmed editable default. The analysis uses raw shaft
streams on a common 50 Hz grid, independently of the master's latest-value
pairing. Both firmware branches therefore produce compatible input logs.

Processing matches the reference: first sample per duplicate timestamp,
engine ceiling 4,000 RPM, derived idler ceiling, centered 0.10 s median,
0.40 s quadratic Savitzky–Golay smoothing, 0.20 s interpolation gap limit,
2 s run separation, driven minimum 50 RPM, CVT range 0.44–3.01 with 5% tolerance,
and assumed efficiency 0.85. Missing samples remain unavailable after smoothing.

Torque/power are **wide-open-throttle map estimates, not measurements**.
The supplied CH440 map covers 2,400–3,600 RPM and holds torque flat outside
that range. The plots flag extrapolated engine samples. Negative encoder RPM
is preserved as in the reference and explicitly flagged: the analysis assumes
forward-positive idler RPM. Fix encoder direction before interpreting results.

“Estimate shaft ratio” assumes full 0.44 overdrive is reached and uses the first
percentile of engine/idler speed ratios. It is marked as estimated; tooth
counts remain the way to confirm the ratio. Calibration cleans with ratio=1
so a previously selected shaft ratio does not bias its idler plausibility gate.
The mock generator uses **2.75**, not the default 1.69565, and does not necessarily
reach full overdrive. Use 2.75 when interpreting the mock's known truth.

## Limits and deliberate robustness changes

The browser handles engine-only logs, missing/invalid channels, short windows,
non-overlapping timestamps and malformed CSV with results or clear errors.
Unlike the original short-window smoothing call, logs with fewer than three
analysis samples do not crash. The binary parser reads the logger's 16-byte
little-endian records, preserves signed values, and warns about trailing bytes.
Per-channel 32-bit millisecond rollovers are unwrapped before sorting.

Quick analysis accepts up to 128 MiB and 500,000 resampled time points. Choose
an individual run for long spans, or use the Python tool for larger logs.
Plots reduce display points when necessary and retain missing-data breaks;
processed CSV contains every analysis point. PNG layout is adapted to the web,
not a pixel-identical Matplotlib rendering. The original log is never modified.

## Validation

```
node tests/test_cvt_core.js
python3 tests/test_cvt_export.py
python3 tests/test_cvt_analysis.py
python3 tests/test_cvt_browser.py
pio run -e MasterStable -t buildprog
```

Python comparison requires the reference requirements. Browser testing also
requires Playwright and Chrome (`CHROME_PATH` overrides the default macOS path).
Tests compare numerical columns with the reference across mock runs, shaft
ratios, selected runs, calibration and engine-only data, plus edge cases.
Offline browser tests cover the completed-log button, worker, all downloads,
run selection, calibration, local uploads, errors and a phone viewport.
Hardware validation of SD download/Wi-Fi still requires the flashed master.

Chart.js 4.5.1: https://www.chartjs.org/docs/latest/getting-started/integration.html
Vendored from https://cdn.jsdelivr.net/npm/chart.js@4.5.1/dist/chart.umd.min.js
License (including bundled @kurkle/color): `src/web/Chart.LICENSE.md`.

## Download input for the original Python script

The **CVT input CSV** button beside CSV downloads `log_XXXX_cvt_input.csv`.
It contains only raw engine RPM (187 / 0x0BB) and idler RPM (185 / 0x0B9), in the
original nine-column format:

```
sample_index,can_id,can_id_hex,signal,node,timestamp_ms,value,units,raw_data
```

Timestamps, signed values, zeros and sensor glitches are preserved. Rows are
numbered consecutively in file order. The Python script performs its own
filtering, resampling and analysis. This input file is different from the
**processed CSV** downloaded from the analysis results page.
The ordinary full CSV also works with the supplied Python; this extra export
simply omits unrelated channels to make testing and transfers easier.

```
python cvt_plot.py log_0001_cvt_input.csv
```

HTTP endpoint: `/api/logs/download?name=log_0001.bin&format=cvt`.
It has the same completed-log checks and download lock as BIN/full CSV.
