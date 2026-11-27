# Drive-control core

One owner task performs all drive transactions. The terminal submits requests to
that owner; a future web interface can use the same API. The ESP32 communicates
with one servo drive, so there are no separate motor and servo subsystems.

See [the source map](../src/README.md) for the current files and command flow.
`drive/control` owns admission, cancellation, results and typed parameter access.
`drive/operations` groups actual mode, movement, brake and disable sequences.
`canopen/drive` handles wire encoding and drive units; `can/transport` handles the
hardware bus. The synchronized feedback cache is shared by RX and the owner.

The cleanup grouped related functions, moved their declarations and renamed the
internal motor API to drive control. It preserved command syntax, request order,
scaling, profiles, timeouts, zero-before-enable and the unconditional 0000 stop
path. It added no browser server, script engine or DAQ runtime connection.

## Frontend use

Include `drive/control.h` rather than console or protocol implementation headers.
After platform CAN initialization, call `drive_control_start()` once. It starts
idle and sends no drive command. An optional `DriveObserver::completed` callback
receives completion results on the owner task; copy results if needed afterward,
and do not block or issue drive I/O inside the callback.

Configure limits and profile through requests first, as with the terminal. Then:

```cpp
DriveRequest request;
request.kind = DriveCommand::Move;
request.values[0] = 0.25f; // Absolute logical motor revolutions.
request.values[1] = 10.0f; // Motor RPM.
DriveReceipt receipt = drive_control_submit(request);
```

A successful receipt means admitted, not completed. Match its ID to a
`DriveResult` from the observer or the latest result in `drive_control_status()`.
Busy rejection returns an error without an operation ID. Status is a copy of
owner state, active/queued ID, limits/profile, last error, latest result and
cached status/position/velocity/current feedback. Each feedback field has its own
receive timestamp; the copy is not a simultaneous physical measurement.

Local idle after boot does not prove an independently powered drive is disabled.
Use stop or live drive diagnostics to establish its reported state.

## Ownership and stopping

Only one drive operation is admitted at a time. Frontends must submit requests;
they must not call `drive/operations` or the CANopen client concurrently.
`drive/operation_context.h` exposes the active operation's cancellation, limits
and wait/error hooks to the C driver and C++ sequences. It is an internal bridge,
not a second frontend API.

`DriveCommand::Stop` and controlword 0000 bypass busy admission and cancel active
or queued work. Pending repeated stops share a receipt ID. A queued request
cancelled before execution gets its own cancelled result; it cannot resume.
Disable writes 6040=0000 before checking status, without mode/velocity guards,
even after an ESP32-only reboot. Read failure, fault or timeout reports disable
unconfirmed; the operator can retry. Normal position completion, failure and
cancellation also end through disable. A disabled statusword proves neither
standstill nor mechanical brake lock.

The terminal retains `stop`, `controller stop`, `test cancel` and
`servo cw 0x0000` as stop commands. `mode manual` remains a confirmation; there is
no automatic mode. The ten-second brake test sends zero velocity before mode 3
and again immediately before enabling. Actual/demand velocity equality is not a
brake-test or disable prerequisite. See [the terminal guide](../TERMINAL.md).

## Parameters and configuration snapshots

Submit `ParameterRead`, `ParameterWrite` or `ParameterSnapshot` with `objects[]`
and `object_count`. `DriveObject` specifies an index, subindex, `ObjectWidth` and
unsigned raw-bit value. Values never pass through float; interpret signed values
using the object's documented type. Width overflow is rejected.

Snapshots read up to eight objects sequentially, with an error and local sampling
timestamp per attempted read. They are not atomic. Cancellation can leave partial
results; only entries below `object_count` were attempted. Failed reads must not
be interpreted as successful zero measurements.

Writes report acknowledgement, not independent readback or mechanical action.
Read afterward to verify a change. A timed-out write can have taken effect.
Generic parameter writes exclude state/motion objects 6040, 6060, 6071, 607A and
60FF, plus EEPROM store/restore 1010/1011. Dedicated operations own those actions.
No automatic retry, rollback, save, factory reset or encoder calibration occurs.
There is no generic parameter-write terminal command yet.

These are CANopen objects, not guessed translations of keypad P-parameters. A
parameter catalogue, engineering units, configuration comparison and persistence
workflow remain part of [the tuning-interface stage](DEVELOPMENT.md).

## Validation

The existing host suite compiles the production owner/sequences against a fake
drive and checks moves, cancellation, unconditional stop/retry behavior, the brake
window and typed parameters. Separate checks cover real C-driver payloads,
transport routing and signed feedback, console parsing/mapping and zero-velocity
preparation. The existing tests were updated for the consolidated sources without
adding new test files or dropping their behavioral assertions.

From the repository root:

```sh
python3 -m unittest discover -s ecvt-node/tests -v
pio run -d ecvt-node -e esp32-p4
```

On the development Mac, prepend
`SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` to the Python
command for the installed compiler/linker combination.

This layout revision has not been flashed or physically revalidated. Earlier
bare-motor observations remain in the [bench report](bench/2026-11-25/README.md).
Host checks and compilation do not establish mechanical behavior.

Validation on 2026-11-27: all seven existing host tests passed and the `esp32-p4`
firmware build succeeded. No new test files were added.
