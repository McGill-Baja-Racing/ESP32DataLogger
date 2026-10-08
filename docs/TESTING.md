# Validate a change

Use this guide after [Contributing](../CONTRIBUTING.md). All commands below run from the repository root unless stated otherwise. Build checks need PlatformIO; host checks need Python 3.9+ and a C compiler named `cc`. Return to the [documentation index](README.md).

## Choose checks by change

| Change | Checks |
|---|---|
| Documentation | Resolve relative links, verify commands/configuration claims, `git diff --check` |
| GPS parser or time conversion | Absolute-time host tests and master build |
| CSV/export behavior | Full and paired CSV host tests and master build |
| UI or master lifecycle | Master build; physical browser/session checks |
| One sensor driver | Affected node build, serial bench values, CAN integration |
| Shared sensor/sampler code | All sensor profiles, including bench; affected hardware |
| Shared protocol | All firmware profiles below, then CAN and exported-data checks |

## Optional host-test setup

These tools are needed for the host test scripts, not for PlatformIO firmware builds. On macOS install Apple's Command Line Tools; on Debian/Ubuntu install Python and `build-essential`. The tests invoke a compiler named `cc` directly and use Unix-style compilation options; native Windows compatibility has not been validated.

For Windows contributors who need these tests, WSL with Ubuntu provides that environment:

1. Follow [Microsoft's WSL installation instructions](https://learn.microsoft.com/en-us/windows/wsl/install): on supported Windows versions, open PowerShell as administrator and run `wsl --install`, then restart if prompted and complete Ubuntu's user setup.
2. In the Ubuntu terminal, run `sudo apt update` and `sudo apt install python3 build-essential git`.
3. Clone this repository inside Ubuntu, or change to your existing checkout through its `/mnt/c/...` path. Work on the branch containing your change.
4. From that checkout's root, run the host commands below. Check `python3 --version` (3.9+) and `cc --version` if a tool is missing.

Keep normal board builds/uploads in your Windows VS Code/PlatformIO setup. WSL is an optional route for host checks; it is not a firmware onboarding prerequisite.

## Hardware-free checks

```bash
python3 master-node/tests/test_absolute_time.py
python3 master-node/tests/test_csv_export.py
python3 master-node/tests/test_paired_csv.py
```

Expect successful results and zero exit status. These compile actual selected C functions into host test harnesses. They cover GPS parsing/calendar validation and CSV behavior, including missing/stale supporting values. They do not verify the full web server, drivers, FreeRTOS scheduling, SD hardware, or CAN timing.

For broad build validation:

```bash
pio run -d sensor-node -e NodeBrake -e NodeMPU -e NodeEncoder -e NodeEngine -e NodeADC -e NodeEngineBench -t buildprog
pio run -d master-node -e Master -e MasterStable -e MasterNoCAN -t buildprog
pio run -d simulators/engine-rpm -e EngineRPMSimulator -t buildprog
pio run -d simulators/wheel-rpm -e WheelRPM_SKF_Simulator -t buildprog
```

Run only affected checks for a focused change; protocol/interface changes warrant the full matrix. Expect every selected environment to report `SUCCESS`. These commands do not upload.

## Known validation gaps

During this documentation update, the full CSV and Powertrain CSV checks passed with a compatible macOS SDK. The absolute-time suite ran nine tests: eight passed and `test_shared_reader_emits_gps_samples_and_synchronizes_utc` failed while compiling its generated harness. That harness lacks time-related declarations and an `ESP_LOGW` definition required by the current absolute-clock source. This is recorded for a future test-code change; the suite must not be described as fully passing.

The first `NodeBrake` build attempt was blocked by the installed PlatformIO/SCons package (`No module named 'SCons.Tool.FortranCommon'`). A clean firmware build remains to be verified after repairing that local tool installation. No hardware acceptance checks were performed for this documentation work. See [Troubleshooting](TROUBLESHOOTING.md#known-validation-issues) for distinguishing these setup failures.

## Serial bench checks

Use [Flashing](FLASHING.md) and [Hardware](HARDWARE.md). `NodeEngineBench` skips CAN and starts locally. For other profiles, temporarily set `SENSOR_SERIAL_TEST=1` for an isolated bench build and restore vehicle mode afterward; do not commit that temporary change as vehicle configuration.

Confirm listed sensor names, signs, units, zero/stopped behavior, known input values, and error handling. Printed values are limited to two per second per sensor, so serial print rate does not prove sampling rate. Use the [RPM simulators](../simulators/README.md) for known rotation signals.

## CAN integration and vehicle acceptance

With normal CAN profiles, a properly connected bus, and a master:

1. Confirm expected node identities and that an idle master leaves nodes idle.
2. Start recording; nodes 1, 3, 4, and 5 should become active when data arrives. Optional ADC monitoring requires an intentional mask update.
3. Exercise both brake channels, all motion axes, signed bearing rotation, engine inputs, and GPS reception. Check value scales against known inputs.
4. Confirm synchronized timestamps and expected signal IDs in full CSV; inspect Powertrain rows when engine and bearing data exist.
5. Repeat START/STOP. Stop must reach idle with closed files; completed downloads should decode cleanly without overwrite.
6. Reboot a node during recording and confirm it rejoins on a beacon. Disconnect a node and confirm offline status after about three seconds.
7. If testing CAN recovery, perform controlled bench fault injection and confirm recovery logs and restored collection; avoid fault injection on an operating vehicle.
8. Repeat SD mounting/start-stop checks and run at least 30 minutes of CAN logging without reset. Inspect CAN/log drop counters, file integrity, and recorded values.

Stop and wait for idle before card removal or power-down. Record board profiles, wiring/calibration assumptions, duration, observations, and remaining failures in the PR. These are required observations to perform, not claims that this checkout has passed vehicle testing.

Next: [Troubleshooting](TROUBLESHOOTING.md) or the relevant component source guide.
