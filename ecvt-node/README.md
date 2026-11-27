# Standalone eCVT motor node

This is the eCVT development project inside the DAQ repository. It controls the
DSY-200C servo drive on the existing **separate 500 kbit/s bench CAN bus**.
It does not consume the shared DAQ protocol, start DAQ sensors, publish to the
master, or use the master's web server. Keep the vehicle DAQ bus disconnected.

The runtime was initially imported unchanged from `ESP32_ECVT` commit
`6e033d5c100ef7d887510a19da2e0f2c0f31b65d`. Source hashes and packaging changes
are recorded in [import provenance](docs/IMPORT_PROVENANCE.json) and the
[validation report](docs/IMPORT_VALIDATION.md).

## Open and build

Open the repository's [VS Code workspace](../baja-daq.code-workspace) and select
**ecvt-node**, environment **esp32-p4**, before building, uploading or monitoring.
You can also open `ecvt-node/` as its own PlatformIO project.
From the DAQ repository root:

```sh
pio run -d ecvt-node -e esp32-p4
python3 -m unittest discover -s ecvt-node/tests -v
```

These commands do not upload. To upload after selecting the intended board:

```sh
pio run -d ecvt-node -e esp32-p4 -t upload
pio device monitor -d ecvt-node -e esp32-p4
```

Keep servo power off during upload. The ESP32 boots idle and requires explicit
terminal commands to enable operation. Opening a serial monitor may restart the
ESP32; an ESP32 reset does not establish the servo drive's state.

Host tests need a C++17 compiler. On the development Mac, the installed macOS 27
SDK and linker are incompatible; the checks were run using
`SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` before the Python
command. Other platforms should follow the repository's [test prerequisites](../docs/TESTING.md).

The owner has since been extracted behind a [motor-control API](docs/MOTOR_CORE.md), with a terminal adapter. The legacy automatic controller, sensors, CSV replay, SD logger and fixed movement presets have now been removed from the project. The current refactor preserves the validated command sequences; it has not yet been physically retested.

## Current standing

| Capability | Status |
| --- | --- |
| Idle boot and manual terminal control | Implemented |
| Servo diagnostics and drive errors | Implemented |
| Zero-speed brake cycle, small position moves and stop during motion | Passed user-run bare-motor checks; [evidence](docs/bench/2026-11-25/README.md) |
| Stop/0000 | Shared unconditional disable path, including busy and MCU-restart cases; host tests cover additional failure/retry paths |
| Legacy controller, sensor acquisition, CSV replay, SD recording and fixed movement presets | Removed; recoverable from Git history |
| Motor-core API, typed parameter access and configuration snapshots | Implemented backend; terminal uses the same operation owner |
| Motor tuning interface and user-authored test scripts | Planned; no browser interface or script editor implemented yet |
| DAQ sensors, shared CAN protocol and master logging | Deferred until standalone control/tuning is ready |
| Loaded eCVT tuning and vehicle deployment | Not validated by the bare-motor checks |

Use [terminal commands](TERMINAL.md), [bench procedure](BENCH_VALIDATION.md),
[source map](src/README.md), and the [development sequence](docs/DEVELOPMENT.md).
The [detailed readiness checklist](docs/DAQ_READINESS.md) retains the earlier
requirements, source checkpoints and remaining validation gaps.

## Preserved build and test baseline

The eCVT project pins pioarduino **55.03.313**, the release already used by the
validated source (ESP-IDF 5.5.6). Existing DAQ projects keep their own platform
pin. This is an intentional isolated toolchain exception, not a DAQ upgrade.
Board `esp32-p4_r3` targets the tested production v3.x P4; pre-v3 chips need a
separate build. `sdkconfig.defaults` preserves the prior effective settings,
including revision selection, 1 kHz FreeRTOS tick and the 2 MB firmware layout.
The layout is not a measurement of the physical flash capacity.

The current host suite exercises the standalone core, actual CANopen serialization,
manual moves and cancellation, parameter access, brake sequencing and terminal.
The import checkpoint and original experiments remain recoverable from Git
history; historical import documents describe that earlier checkpoint.

Known limitations include the first-command FF serial byte, noisy repeated brake
status messages and incomplete restart/fault hardware coverage.
Drive parameters are not reset by moving this project. Save a read-back export
of the working motor configuration before changing tuning.
