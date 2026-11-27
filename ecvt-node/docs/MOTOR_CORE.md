# Motor-control core

The core owns all drive operations for one servo (the existing node ID 1 setup).
It does not include terminal, web, DAQ sensor, legacy runtime or SD headers.
Platform startup still initializes CAN/receive services before starting the core.

## Modules

| Module | Responsibility |
| --- | --- |
| `motor/requests.h` | Frontend-independent operations, raw typed objects, admission receipts and completion results |
| `motor/motor_core.cpp` | Single owner task, admission, cancellation, operation IDs, state/status and completion notifications |
| `motor/drive_operations.cpp` | Existing position preparation and zero-torque diagnostic sequences, with explicit profile inputs |
| `motor/parameters.cpp` | Owner-only 8/16/32-bit object access and bounded sequential snapshots |
| `motor/runtime.h` | C-compatible cancellation, limits, wait and error hooks used by the driver/runners |
| `motor/limits.h`, `operation_gate.h` | Existing motion bounds and cancellation generations |
| `servo/` | CANopen serialization, disable/zero-velocity helpers and synchronized feedback |
| `experiments/` | Existing finite position/brake diagnostics, using neutral motor operations |
| `app/ecvt_app.cpp` | Terminal adapter: maps parsed commands, prints status/help and preserves legacy callback names |
| `legacy/legacy_runtime.cpp` | Optional automatic-controller, ADC/replay and local recording services |

The drive command bytes, limits, position units, profiles, enable sequences,
zero-speed brake window and unconditional 0000 stop policy are retained.
The manual core can run with no legacy services. Selecting automatic mode does
not start anything; automatic start without an installed service refuses before
position preparation or enable.

## Frontend use

Include `motor/motor_core.h`, not `app/runtime_internal.h` or the console parser.
After platform CAN initialization, call `motor_core_start()` once. Optional
`MotorCoreServices` connect application recording, automatic control and result
notifications. An empty service configuration supports manual operations without
linking the terminal, sensors, CSV replay or SD implementation.

```cpp
MotorRequest request;
request.kind = MotorCommand::Move;
request.values[0] = 0.25f; // Absolute logical motor revolutions.
request.values[1] = 10.0f; // Motor RPM.
MotorReceipt receipt = motor_core_submit(request);
```

Configure limits/profile through requests first, as with the existing terminal.
A successful receipt means admitted, not moved or completed. Match its ID to a
`MotorResult` from `MotorCoreServices::completed`, or the latest result in
`motor_core_status()`. The callback runs on the owner; copy its result before
returning if another task needs it, and do not block or issue drive I/O there.
The status includes owner state, busy/queued request ID, latest synchronized
position/velocity/status/current feedback with timestamps, limits/profile,
last error, optional recording state and the latest completion result.
It is a task-safe copy, not an atomic measurement of the entire physical drive.
Local idle state after boot does not prove that an independently powered drive
is disabled; use the stop request/live drive diagnostics.

Only one drive operation is admitted at a time. Busy rejection returns an error
and no operation ID. Stop and controlword 0000 bypass busy/mode admission, cancel
queued/active work, and use the existing disable-first path. Pending repeated
stops coalesce to the same receipt ID. A queued request cancelled before execution
also gets a cancelled result. No queued move resumes after stop.

New frontends must submit requests; they must not call the CANopen driver,
parameter helpers or operation runner concurrently. `MotorCommand::Stop` is the
frontend-independent way to stop. The terminal retains all existing command
names and parsing rules, including `stop`, `controller stop`, `test cancel`, and
`servo cw 0x0000`.

## Parameters and configuration snapshots

Use a `MotorRequest` with `ParameterRead`, `ParameterWrite` or
`ParameterSnapshot`, plus `objects[]` and `object_count`. Each object has an
explicit index, subindex, `ObjectWidth` and unsigned raw-bit value. Values never
pass through a float; signed values are interpreted using the object's known
type. Width overflow is rejected, not truncated.

A snapshot reads up to eight objects in order, with an error and local sample
timestamp for every attempted read. It is sequential, not an atomic configuration
snapshot. Cancellation can leave a partial result; only entries below
`object_count` were attempted. Failed reads have explicit errors and must not be
interpreted as successful zero-valued measurements.

Writes report acknowledgement, not independent readback or mechanical action.
Submit a subsequent read to verify a change. A timed-out write can have taken
effect. No automatic retry, rollback, EEPROM save, factory reset or encoder
calibration is performed. State/motion objects (6040, 6060, 6071, 607A, 60FF)
and EEPROM store/restore (1010/1011) are excluded from this generic tuning-write
path; dedicated motor operations own them. No new raw-write terminal command is
introduced by this refactor.

These are CANopen objects, not guessed translations of keypad P-parameter
numbers. A supported parameter catalogue, units, before/after comparisons and
explicit persistence workflow belong to the next tuning-interface stage.

## Recording and automatic-control adapters

`motor_core_record_command()` gives the optional recorder the same commanded
position/speed as before. The legacy adapter adds its legacy input fields to the
existing binary log. Received servo feedback still enters that logger from the
CAN receiver; current TPDO4 is now also available in the independent feedback
snapshot. No disk format, sensor conversion or controller calculation changed.

Recording start/stop are separate, thread-safe application-service calls
(`motor_core_start_recording/stop_recording`) so recording remains independent
of motor admission. Their hooks and status hooks may run on a frontend task;
they must not touch the drive. The legacy adapter retains its asynchronous SD
writer. Automatic/replay shutdown still pauses inputs before disable and waits
for replay cleanup afterward, so SD/replay work does not delay sending 0000.

## What this refactor does not add

The existing position moves and diagnostic velocity/torque sequences remain the
implemented motion set. It does not add unrestricted velocity/torque presets,
a general script interpreter, a browser server, or a DAQ input adapter. Those
features can now use one operation API instead of implementing separate drive
control paths. See [development sequence](DEVELOPMENT.md).

## Validation

The host harness compiles the complete production owner, position runner,
parameter module, brake/disable helpers and feedback module against a fake drive,
without any terminal, legacy sensor or SD implementation. It covers completed and
cancelled moves, queued cancellation, stop from a rebooted owner/busy/automatic
state, coalesced stops, acknowledgement/read failures, faults/timeouts, mode-3
zero-before-enable, the ten-second brake window, typed object values/overflow,
snapshots and cancellation, and current-feedback capture.

A separate harness compiles the real terminal adapter and parser and checks that
help/status/test-list/log commands do not turn into motor requests, stop aliases
map correctly, and object reads retain their types. Existing real-driver payload,
calculation/migration and runner checks remain in the suite. Run:

```sh
python3 -m unittest discover -s ecvt-node/tests -v
pio run -d ecvt-node -e esp32-p4
```

This refactored revision has not been flashed or physically revalidated. Prior
bare-motor evidence remains in the [bench report](bench/2026-11-25/README.md).

Recorded validation on 2026-11-26: all **12 host tests passed**, including
76,403 differential controller calculation cases; the `esp32-p4` firmware
build succeeded. Master/sensor/protocol/simulator files were unchanged.
