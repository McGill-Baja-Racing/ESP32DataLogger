# Terminal operation

This is bench firmware using the existing 500 kbit/s servo/sensor map, ADC RPM inputs and optional SD CSV replay. The future 1 Mbit/s DAQ input bus and centralized logging are not implemented here. Boot initializes receive/storage services but does not send servo commands or start control/replay.

Build/upload using the existing `esp32-p4` environment. Open a serial monitor at 115200 baud; commands are newline-delimited. PlatformIO's `send_on_enter` filter allows editing a line before sending it.

The UART console now uses interrupt-driven reception with a 2048-byte RX buffer instead of polling the hardware FIFO. PlatformIO sends LF line endings; the firmware accepts CR, LF, and CRLF. Close and reopen the monitor after updating its configuration. Local monitor echo shows typing, while `Received: ...` shows the command that actually reached the firmware. Backspace/Delete remove the preceding character; arrow-key escape sequences and other control/non-ASCII bytes reject the entire line. Invalid input prints received bytes in hexadecimal for diagnosis. A rejected or oversized line is never executed as a truncated command. The buffered UART change addresses a possible input-loss path; physical serial reliability still needs confirmation.

## Commands

| Command | Effect |
|---|---|
| `help` / `status` | Command reference / mode, operation, latest servo feedback age, logging and loss count |
| `servo ping` | Read actual operating mode `6061h:00` over CANopen; no setup, enable or motion commands |
| `servo inspect` | Read status, user position, signed raw velocity, encoder resolution, gearing/position factors and digital outputs/mask; no writes |
| `servo errors` | Read current error, error register, up to five history entries and DC-link voltage; no reset/history clear |
| `servo read <index> <sub> u8/u16/u32` | Read one object; decimal or 0x hex address/subindex; strict width validation |
| `servo cw 0x0000/0x0006/0x0007/0x000F` | 0000 is an unconditional disable/cancel request, including while busy; nonzero words are supervised single state steps |
| `servo velocity-zero` | With the drive disabled and alarm-free: write target velocity 0, select mode 3, verify actual mode/zero target; no controlword or enable |
| `servo position-mode` | With the drive disabled and alarm-free: select and verify profile-position mode 1; leaves any stored position target unchanged and sends no controlword |
| `servo torque-zero-test` | Experimental brake-voltage observation: select profile torque mode, verify target torque 0, enable for 20 s, then write zero again and disable |
| `servo reset-fault` | Explicit idle-manual bench reset: 6040=0080 then 0000; no enable/target; verify state afterward |
| `servo hold-test` | Experimental stationary hold measurement: 1 s active wait, up to 10 s measurement, then disable/status confirmation; no three-turn target |
| `servo brake-test` | Unloaded zero-velocity state test: 0006 -> 0007 -> 000F, 10 s enabled, then 0000 |
| `servo brake-restore` | Send 0000 and confirm drive disabled, even without a remembered test |
| `mode manual` / `mode auto` | Select mode while stopped; no motion starts |
| `limits <min_rev> <max_rev> <max_rpm>` | Set explicit logical travel and speed bounds for this boot |
| `profile <accel_rpm_per_s> <decel_rpm_per_s>` | Set the manual move profile; initially the existing value 2300 for both |
| `move <logical_rev> <rpm>` | Prepare/enable position mode, move, wait for arrival or 20 s, then send 0000 and confirm drive disabled |
| `test list` | Print the available preset targets and speeds |
| `test run position-step` | Legacy argument 100 rev at 1000 rpm, accel 500/decel 100, 12 s window |
| `test run position-profile` | 75 rev at 2000 rpm, accel/decel 1666, target arrival or 20 s |
| `test run step-response` | +10 then -10 rev at 2000 rpm, accel/decel 1000, 3 s per target |
| `test run three-revolutions` | Approved unloaded bench test: +3 motor revolutions from current position at 60 rpm, drive-positive direction; verifies scaling before enable |
| `controller start` | Start the legacy controller inputs/loop in auto mode; optional `/sdcard/lap_data.csv` replay starts anew |
| `stop`, `test cancel`, `controller stop` | Cancel active/pending work; always send 0000, then check drive status |
| `log start` / `log stop` | Start a new local SD file / drain queued records and close; independent of motion |

Configure limits from the actual test fixture before starting general motion commands. There are no default mechanical travel bounds. Presets must fit those limits in full; step-response checks both signs before starting. The exception is the explicitly approved `three-revolutions` bench preset below, which establishes its measured start/end envelope if no limits have been set. Automatic mode checks the legacy -6.9 to 0 logical range and speed up to 3000 rpm used by its state-entry actions, and guards each position/speed command. Limits and profile settings are not persisted across reboot. Full-rate velocity and torque experiments remain catalogued but are not exposed until their cancellation behavior is defined.

## Approved three-revolution bench test

Hardware commissioning is **not complete**. Live tests on 2026-11-20 produced overload and position-deviation faults with very little shaft travel, including a hold-only test. The subsequent [November 25 bench report](docs/bench/2026-11-25/README.md) records successful bare-motor brake, small-position and stop checks after tuning correction. The [November 20 report](docs/bench/2026-11-20/README.md) remains historical context; loaded operation and larger presets are still unvalidated.

The `three-revolutions` preset requires idle manual mode with the drive disabled, no pending brake override restoration, and output mask zero. It reads current position and 6091 gearing before setup. The [DSY conversion equation](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=43) must match the existing 10000 command-units-per-motor-revolution conversion. Unsupported scaling, count overflow, precision loss, fault state or failed reads refuses the test. Target is the measured C+30000 at 60 rpm in the drive-positive direction. If the user has not explicitly configured limits, the bench preset recomputes its start/end/60 rpm envelope each run; explicit limits remain binding. Existing manual acceleration/deceleration values apply. No software zero is recaptured.

Setup checks the drive state after each enable controlword. For this bench sequence, it initially enables with Halt, writes/latches the measured current-position target, verifies the setpoint acknowledgement, and checks 6062 demand against the hold target before clearing Halt. It then enters active hold (000F) and waits 1000 ms, matching the old test's explicit brake-release wait. A demand mismatch during that wait refuses the move. One command-count tolerance matches the existing position completion precision. These are commissioning guards, not a physically validated guarantee of stationary holding or confirmed brake release.

After the wait, the three-turn preset sends its absolute target and requires fresh position feedback plus target-reached status; timeout remains 20 s. It records voltage/status/position/velocity and reads the fault code if a drive fault occurs. The hold-only preset instead spends up to 10 s reading current, voltage, outputs and demand, without sending the three-turn target. It aborts on fault, stale/disabled status, read failure, cancellation or demand mismatch.

Both bench presets, other position moves/presets, automatic-controller completion and explicit stop now send `6040=0000` directly, then check Switch on disabled. No exact-zero velocity or position-mode prerequisite blocks disable. This replaces Halt-and-hold completion. The drive's configured stop/BK behavior determines its physical deceleration and brake sequence; the firmware does not claim standstill or mechanical lock from a disabled statusword.

The November 20 fault/reset observations remain historical evidence. A cleared statusword fault bit alone does not prove all drive alarms cleared; use error/register/history diagnostics. Software stop cannot act through a failed CAN connection.

## Communication-only test

For the user-selected zero-velocity state-step comparison, first power-cycle/inspect the drive to establish an alarm-free disabled state, then use `servo velocity-zero`. It writes 60FF=0 **before** selecting 6060=3, polls 6061 for mode 3, and verifies 60FF=0, error register=0 and Switch on disabled. A cancelled/failed preparation can leave a partial configuration but sends no controlword. The command does not alter position targets, send NMT/PDO remapping, clear a fault, save parameters, or automatically progress through 0006/0007/000F. Each subsequent state step remains an explicit operator command. Enabling mode 3 still enables servo torque; it is not an independent torque-off brake release.

The single-step controlword command is for supervised unloaded bench work. The parser accepts only 0000, 0006, 0007 and 000F; it does not expose setpoint-trigger words or fault-reset words. Nonzero direct steps are refused while operation is already enabled. `servo cw 0x0000` and `stop` always request disable, including while busy or in automatic mode. Direct 000F requires switched-on state and no error register. In mode 3 it sends another explicit zero target velocity and verifies that target before enable. The user explicitly authorized position mode 1 for the original state-only experiment, preserving its position/trajectory registers; mode 3 retains zero target-velocity readback; demand and actual velocity are not prerequisites. Enabling position mode can activate an existing target even though this command writes no new target. A 0006/0007 request does not clear a latched fault, change operating mode, remap PDOs, send NMT, or follow automatically with enable. Read the returned status rather than assuming the state changed. The first live 0006 step on 2026-11-20 was acknowledged and returned statusword 0231 (Ready to switch on, operation disabled, no fault).

After uploading this version and reaching the idle prompt, power the drive normally on the existing servo bench CAN bus (500 kbit/s, node 1), then enter `servo ping`. A successful response is `Servo replied: node=1 object=0x6061:00 mode=N`. The value N reports the drive's current mode; it need not be 1 for this probe to succeed. This confirms a fresh CANopen read response from the drive without changing mode or enabling the motor. It does not configure PDOs, start NMT, establish position zero, test braking, or prove that drive faults are absent. No movement limits are required for this read-only command. `status` alone only reports previously received data and is not an active CAN test.

The probe uses the same motor-owner task and is refused while another operation is busy, preventing overlapping SDO transactions. It is permitted for diagnosis in STOP-UNCONFIRMED state but does not clear that state. Probe failure alone does not mark the drive commanded or invoke a Halt/enable sequence. Old responses whose receive timestamps precede the new request are rejected.

For the unloaded motor's brake investigation, enter `servo inspect` and save the entire output before attempting an override. This sends SDO upload requests through the same owner task and never changes drive state or output masks. It now includes actual user position (6064), encoder resolution (608F), electronic gearing (6091) and position factors (6093), along with status, velocity and outputs. Each read is reported independently, including unsupported/failed reads; a failed read is never printed as a zero value. The operation-enabled and fault flags are reported from the statusword. They are not physical torque/brake measurements, and the readings are sequential rather than an atomic snapshot. The EDS does not define which output bit controls the brake. Successful reads establish object availability and a baseline, not brake-control semantics.

## Unloaded brake state-sequence test

`servo brake-test` now uses the drive-managed brake sequence requested by the user: `6040=0006 -> 0007 -> 000F`, ten seconds enabled, then `6040=0000`. It replaces the unverified `60FE` output-bit experiment and never writes output masks. `000F` enables servo torque; this is not a torque-off brake release.

The command requires idle manual mode before other motion, a disabled/alarm-free drive. It writes target velocity `60FF=0` before selecting velocity mode `6060=3`, then verifies actual mode, zero target and disabled state. This avoids activating a retained position target. Velocity mode 3 remains selected afterward; no position target, nonzero speed, fault reset, NMT, PDO remapping or parameter save is sent.

Each controlword is acknowledged and the expected statusword state must be observed within one second. Immediately before `000F`, the test sends another explicit `60FF=0` target-velocity write and verifies the zero target before enabling. Failure or cancellation skips enable and attempts `0000`. Target velocity, mode and error register are checked before each step and throughout the nominal ten-second enabled window. The window starts after Operation enabled is confirmed. Communication/check delays can extend it. Cancellation or an error ends the window and attempts `0000`; cleanup ignores cancellation and confirms Switch on disabled. Communication loss cannot guarantee physical disable or brake application.

If disable cannot be confirmed, motion/settings remain blocked. `servo brake-restore` now retries `0000` and disabled-state verification; it does not write digital outputs. Pending cleanup is RAM-only: restarting the P4 is not proof that the drive was disabled. Physical brake release/reengagement must be observed; software state alone does not confirm it.

For a logged session from `ecvt-node/`, substitute the current port and a new log filename:

```sh
/Users/remylaurendeau/.platformio/penv/bin/python tools/bench_serial.py --port /dev/cu.usbmodem5CF71544971 --log /private/tmp/ecvt-brake-state-test.jsonl --allow-brake-probe
```

The tool sends nothing on connection, though opening the port may reset the P4. With the motor secured and uncoupled/unloaded, enter these commands individually, checking each response:

```text
status
mode manual
servo ping
servo inspect
servo errors
servo brake-test
servo inspect
servo errors
```

Proceed to the test only after the diagnostic reads confirm the disabled, alarm-free baseline. Observe release during the enabled window and reengagement after `0000`. Actual velocity is deliberately not read or used to abort this brake test, including during disable confirmation. Completion confirms drive state, not standstill. On unconfirmed disable, use `stop`, `servo cw 0x0000`, or `servo brake-restore`. Enter `:quit` to close the logger. This option permits the bounded test, not individual controlword steps or motion presets.

Position and rpm labels use the existing serialization: 10000 counts/revolution and rpm * 10000 / 60 for profile speed. Verify the drive's electronic gearing before interpreting those as physical motor revolutions. No output-shaft/50:1 gearbox conversion is added. The driver rejects physical count overflow after its logical zero offset is applied. Automatic state logic retains its original zero recapture behavior; terminal stop does not invoke it.

## Start, stop and ownership

One motor-owner task performs all setup, tests and automatic control. Commands other than help, status, stop/0000 and logging are rejected while it is occupied. Stop invalidates any pending operation; no queued move resumes afterward. Complete a stop before mode changes; every mode requires a later explicit motion/start command.

Position setup maps position plus speed consistently for all three presets, including the legacy step-response source that previously mapped position alone while sending eight bytes. Statusword TPDO1 is explicitly configured with event transmission, 20 ms inhibit and a 30 ms event timer, so stop confirmation need not rely on a status change. Setup seeds the measured current position before enabling with Halt set, to avoid executing a prior target on enable. Missing initial position feedback prevents movement.

`stop`, `controller stop`, `test cancel` and `servo cw 0x0000` all invalidate pending/active work and request the same disable operation. The owner sends `6040=0000` **before** any status read, mode check or replay/SD cleanup wait. It does this even on a fresh ESP32 boot, when this firmware never enabled the drive, in automatic mode, while busy, or when a prior disable failed. It never sends Halt or an enable word as part of stopping. These commands are serviced by the owner after its current bounded transaction or cooperative operation yields; serial acceptance is not proof that the CAN write has completed.

After the write, the owner polls `6041` for up to one second for Switch on disabled, using mask `004F -> 0040`. Mode, demand velocity, actual velocity and cached feedback are not stop prerequisites. Even a missing write acknowledgement is followed by a state read: if live status confirms disabled, that state is reported with the acknowledgement failure kept visible. A failed read, drive fault or state timeout reports `DISABLE UNCONFIRMED`; it does not claim standstill, physical brake engagement or cleared drive faults. New movement/settings stay blocked until an explicit disable retry succeeds. No automatic restart follows a successful disable. Normal position/automatic runner completion also uses this path.

The physical brake remains **UNVERIFIED** in status. `0000` requests the drive's configured stop/disable/BK sequence; the drive's actual configuration determines deceleration and brake timing. This bench policy is separate from the vehicle brake-input state and sensor-loss policy still to be designed. No mechanical zero is captured or return-to-zero commanded by stop.

The brake test no longer requires zero demand or actual velocity: it verifies commanded target zero, operating mode and drive errors, and retains its ten-second enabled window. This is a deliberate bench simplification, not vehicle failsafe validation.

The CAN driver uses persistent TX buffers and finite waits (100 ms per transmit/completion stage), and propagates errors instead of resetting the chip. A pending frame retains its buffer on timeout; a new frame cannot overwrite it until completion. Servo feedback freshness uses receive-callback timestamps, so time in the software dispatch queue does not make an old frame fresh. This cannot cancel a frame already accepted by hardware or guarantee stopping during bus failure. Missing confirmation remains explicit.

## Logging and validation limits

Logging is opt-in and can be started before a test or during an operation. `log start` requests opening asynchronously on the writer; wait for its success message before starting a test if the whole test must be recorded. `log stop` cancels a pending start or closes after queued records drain; retry `log start` after closure if a prior session is still draining. Recording can restart without reboot. No SD card or logging failure prevents terminal/control operation. Filesystem work runs on the writer so console stop handling never waits for an SD file open/close. Admission never waits behind SD writes; queue/admission losses are counted, and short writes stop recording without stopping motor control. Counts are cumulative for the boot and reported by `status`.

This is the original local 44-byte record format. It does not yet implement complete command/sensor provenance, every setup/controlword event, acknowledgements, master telemetry, or local fallback reconciliation from the integration plan. The legacy sensor snapshot behavior and automatic control thresholds remain; this checkpoint does not introduce the future missing-sensor failsafe.

Host checks cover parsing, motion bounds, exclusive command admission, cancellation generations, stop from enabled/reboot/busy/automatic states, failed writes/reads and retries, wire serialization, error propagation and pending TX storage lifetime. Differential calculation checks continue to compare 76,403 cases with the original. Hardware checks are still required for drive state transitions, gearing, stop behavior, brake timing, terminal responsiveness and repeated SD sessions. Nothing was flashed during implementation.
