# Troubleshooting

Use this guide after [Getting started](GETTING_STARTED.md) or [Flashing](FLASHING.md). Start with the symptom and collect evidence before changing configuration. Return to the [documentation index](README.md).

| Symptom | First checks | Evidence to share |
|---|---|---|
| `pio` command missing | Open PlatformIO Core CLI in VS Code; extension and ordinary terminal paths differ | Terminal type and tool version |
| Build fails | Check working directory, project/environment pair, first error, network/package downloads; use the project configuration's toolchain rather than changing versions blindly | Exact command, first error, OS, PlatformIO version |
| Host tests fail to start | Check Python 3.9+ and `cc`; Windows users can use Linux/WSL for these host harnesses | Versions and compiler/test error |
| No USB port or serial output | Use a data cable, check power and `pio device list`, select the correct port, close other monitors, use 115200 baud; allow sensor startup delay | Port list, board role, reset/startup log |
| Simulator build unexpectedly asks for a port | Use explicit `-t buildprog`; simulator defaults include upload/monitor | Command and selected target |
| Node sends no measurements | Confirm vehicle versus serial-test profile, active recording, correct node role, CAN transceiver/pins, common ground, bitrate, termination; inspect local sensor init errors | Node startup log, master status, error/drop counters |
| RPM is zero or wrong | Confirm simulated/conditioned input, engine accepted interval range, bearing channel order/counts; check overflow reports | Known input speed, mode, wiring, output and counters |
| GPS data/time absent | Confirm UART direction/pins/baud, power and reception; telemetry needs valid active RMC data; UTC also needs a valid date/time | Startup/parser messages and receiver conditions |
| SD mount fails | Check card presence, power and FAT formatting; current firmware does not auto-format on failure | Serial mount error, card/filesystem details |
| Web interface unavailable | Connect to `BajaDAQ`, open `http://192.168.4.1`, inspect ESP-Hosted/C6 startup compatibility; keep serial collection status separate | Network startup error, profile, locked dependency information |
| Live graphs unavailable | Ensure recording is active, explicitly enable live view, check whether another viewer owns the lease | Status, browser action, live endpoint response |
| Absolute time blank / Powertrain empty | Check UTC companion and GPS synchronization; Powertrain requires eligible bearing data before engine samples | File sizes, selected export, relevant full-CSV rows |

Do not enable SD formatting as a routine diagnostic on a card containing useful recordings; the flag permits erasure. Preserve logs and configuration evidence when reporting a failure.

Use [Hardware](HARDWARE.md) for exact pins and assumptions, [Protocol](PROTOCOL.md) for signal identities, [Log format](LOG_FORMAT.md) for export rules, and [Testing](TESTING.md) for expected outcomes.

## Known validation issues

The documentation validation encountered the following issues; none was repaired by changing firmware or tests:

- On the validation Mac, the default SDK/linker pair rejected `arm64e.x1` in `MacOSX27.0.sdk`. Using the already installed compatible SDK via `SDKROOT` allowed CSV checks to run. Repair/update the local developer tools or select a compatible installed SDK; do not copy a machine-specific SDK path into project configuration.
- The absolute-time suite's generated GPS pipeline harness lacks definitions needed by the current absolute-clock implementation (`time_t`, `suseconds_t`, `settimeofday`, and `ESP_LOGW`). This requires a separate test-harness update. A parser test result alone does not replace the failed integration check.
- The installed PlatformIO/SCons build failed with `No module named 'SCons.Tool.FortranCommon'`. Check/reinstall the affected local PlatformIO tool package before investigating sensor source changes. Keep the project's configured versions rather than changing dependency pins to hide a local package failure.

Use [Testing](TESTING.md#known-validation-gaps) for the recorded check outcomes.
