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
| `app/ecvt_app.cpp` | Terminal adapter: maps parsed commands, prints status/help only |

The drive command bytes, limits, position units, profiles, enable sequences,
zero-speed brake window and unconditional 0000 stop policy are retained.
The firmware contains no automatic controller, DAQ/sensor acquisition, CSV replay,
SD recorder or fixed movement presets. `mode manual` remains a compatibility
confirmation; there is no automatic mode.

## Frontend use

Include `motor/motor_core.h` rather than the console parser.
After platform CAN initialization, call `motor_core_start()` once. Optional
`MotorCoreServices::completed` observes completion results. There are no automatic-
control, acquisition or recording service hooks.

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
last error and the latest completion result.
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
frontend-independent way to stop. The terminal retains the stop aliases, including `stop`, `controller stop`, `test cancel`, and
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
cancelled moves, queued cancellation, stop from a rebooted owner or busy
state, coalesced stops, acknowledgement/read failures, faults/timeouts, mode-3
zero-before-enable, the ten-second brake window, typed object values/overflow,
snapshots and cancellation, and current-feedback capture.

A separate harness compiles the real terminal adapter and parser and checks that
help/status commands do not turn into motor requests, stop aliases
map correctly, and object reads retain their types. Removed automatic/logging/preset commands are rejected by parser tests. Real-driver
payload and brake/velocity preparation checks remain in the suite. Run:

```sh
python3 -m unittest discover -s ecvt-node/tests -v
pio run -d ecvt-node -e esp32-p4
```

This refactored revision has not been flashed or physically revalidated. Prior
bare-motor evidence remains in the [bench report](bench/2026-11-25/README.md).

The earlier 12-test import/refactor suite included legacy calculation checks. Those
checks and reference fixtures were removed with that implementation. Current
validation covers the firmware that remains.

Bare-core validation on 2026-11-27: all **7 host tests passed** and the
`esp32-p4` firmware build succeeded. The CAN receiver test compiles the production
transport and checks startup, SDO routing, signed TPDO decoding, timestamps and
short/unrelated-frame rejection. No master, sensor or shared protocol files changed.
