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
- Figure PNG, four-column processed CSV, and a text summary with settings.
- All runs or one run, an editable RPM ceiling and shaft ratio, and an optional
  ratio estimate. Local BIN/CSV files can also be opened on the same page.

## Inputs and assumptions

The browser port follows the supplied `tools/cvt_reference/cvt_plot.py`.
The supplied analysis and mock generator are kept for desktop use and numerical
comparisons. The analysis default shaft ratio has been updated to 3.389286.
The four Python dependencies are listed in
`tools/cvt_reference/requirements.txt`.

Engine input is CAN 0x0BB (node 5); idler input is 0x0B9 (node 4). Extra spark,
wheel-edge and master-paired records are ignored. Driven RPM = idler RPM ×
**3.389286**, the user-confirmed editable default. The analysis uses raw shaft
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
The mock generator uses **2.75**, not the default 3.389286, and does not necessarily
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
processed CSV contains only complete analysis points with GPS speed. PNG layout is adapted to the web,
not a pixel-identical Matplotlib rendering. The original log is never modified.

## Validation

```
node tests/test_cvt_core.js
python3 tests/test_cvt_export.py
python3 tests/test_paired_csv.py
python3 tests/test_cvt_analysis.py
python3 tests/test_cvt_browser.py
python3 tests/test_live_rpm_views.py
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

## Paired RPM and vehicle speed download

**Powertrain CSV** downloads `log_XXXX_rpm_paired.csv`, with exactly:

```
Timestamp,Engine RPM,Wheel RPM,Car Speed (km/h)
```

Timestamp is the master-clock time in milliseconds. Every data row has both
an engine and wheel value. This export matches recorded engine RPM (0x0BB)
with master-paired wheel RPM (0x0BD) at the same timestamp; that wheel value
was selected by the master from a node 4 reading at most 100 ms old.
Unrelated CAN records can interleave. Unmatched engine/wheel records are
omitted; a value is never invented or carried forward to fill a missing half.
Actual zero RPM is valid, including engine=0 with a moving wheel. Signed
wheel RPM is preserved. The formatter streams in constant memory.

Car Speed (km/h) uses the latest GPS speed record encountered before the
pair is emitted, divided by 100 and formatted to two decimal places. Its
GPS timestamp must be no later than the pair timestamp (with clock rollover
supported). The field is blank when no eligible reading is available. GPS
speed is held between updates without interpolation or an age cutoff.

This format uses the recorded pairs, not a new resampling or interpolation.
Logs without complete recorded pairs (including older logs without 0x0BD)
produce a header-only CSV. Use full CSV/CVT input CSV to inspect their raw
channels. The original Python script still expects **CVT input CSV**, not
this compact four-column format.

Endpoint: `/api/logs/download?name=log_0001.bin&format=paired`.

## Live RPM display choices

The Live Data selector offers three independent views of node 4:

| View | Calculation |
| --- | --- |
| Raw Bearing RPM | Recorded bearing RPM |
| Secondary RPM | Raw bearing RPM × 3.389286 |
| Wheel RPM | Raw bearing RPM ÷ 3.589 |

Select any combination, including all three plus engine RPM within the existing
four-chart limit. Derived numeric labels show one decimal place; plots retain
full calculation precision, including signed/zero readings. Each view has its
own pause, zoom and axis controls. The displayed formulas identify the conversion.
The engine-paired bearing stream is labeled **Engine Paired Bearing RPM** to
identify it as unscaled rather than physical wheel RPM.

These conversions affect the live browser display only. Raw CAN/SD readings
and CSV downloads keep their recorded values, including the existing paired
CSV's `Wheel RPM` column (the unscaled bearing reading paired by the master).
The corrected **3.389286** also applies to the Python/browser CVT analysis
shaft-ratio default; the browser analysis field remains editable. The standalone
mock generator keeps its deliberate 2.75 fixture ratio for regression testing.

## Processed CSV format

**Download processed CSV** contains exactly:

```
engine_rpm,bearing_rpm,gps_speed_kmh,timestamp_ms
```

Engine and bearing RPM are smoothed 50 Hz analysis samples for the selected
run. GPS speed comes from recorded CAN 0x700 values divided by 100 to obtain
km/h. Each sample uses the most recent GPS reading at or before its timestamp,
held between GPS updates without an age cutoff, like the paired CSV export.
Rows missing any of the four values are omitted; actual zeros remain valid.
Logs without GPS speed produce a header-only processed CSV. Use the full CSV
or binary log as input: the RPM-only CVT input CSV has no GPS records.

`timestamp_ms` is logger time in milliseconds. `bearing_rpm` is the unscaled
bearing RPM. Engine RPM ceilings above 4000 are rejected. Input samples
above the configured ceiling are rejected before processing; smoothing results
above that ceiling are marked unavailable and omitted from the CSV. The export
also independently excludes engine RPM above 4000, without clamping it to 4000.
