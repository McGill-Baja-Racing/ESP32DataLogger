# Standalone motor terminal

Open `ecvt-node/` in the DAQ repository, select `esp32-p4`, build/upload, then
open the serial monitor at 115200 baud. The firmware starts idle. It has no web
server, automatic controller, DAQ inputs, CSV replay or SD logger.

| Command | Action |
| --- | --- |
| `help`, `status` | Command list; local owner state and latest servo feedback |
| `stop` | Cancel pending/active work, send `6040=0000`, then check drive disabled |
| `mode manual` | Confirm manual operation; no drive writes |
| `limits MIN MAX RPM` | Per-boot absolute logical motor-revolution bounds and maximum RPM |
| `profile ACCEL DECEL` | Manual acceleration/deceleration in motor RPM/s |
| `move POSITION RPM` | One absolute position move; disable on completion, error or cancellation |
| `servo ping` | Read current operating mode |
| `servo inspect` | Read status, position, velocity, gearing, factors and output objects |
| `servo errors` | Read current fault, error register/history and DC-link voltage |
| `servo read INDEX SUB u8/u16/u32` | Read one CANopen object; decimal or hex addresses |
| `servo velocity-zero` | Disabled drive only: zero velocity target, select mode 3, verify; no enable |
| `servo position-mode` | Disabled drive only: select mode 1; no target or enable |
| `servo cw 0x0000/0x0006/0x0007/0x000F` | One explicit controlword; 0000 bypasses enable guards |
| `servo brake-test` | Zero-velocity mode, 0006 → 0007 → explicit zero target → 000F, ten seconds, 0000 |
| `servo brake-restore` | Retry disable and confirm drive state |
| `servo reset-fault` | Idle bench before motion: reset a reported fault, clear reset bit; no enable |
| `servo torque-zero-test` | Experimental mode-4 zero-torque enable window, up to 20 seconds, then disable |

`controller stop` and `test cancel` remain stop aliases. `mode auto`,
`controller start`, `log start/stop`, `test list`, all old fixed movement presets
and `servo hold-test` are removed and rejected. The `mode=manual` status label
describes the application; `servo ping` reads the drive's actual operating mode.

## Small moves

First inspect the stationary drive. Commands use **10000 drive user counts per
motor revolution**, motor RPM and RPM/s. Verify electronic gearing; no 50:1
output-shaft conversion is applied. At a fresh boot, absolute logical position
is `6064 / 10000`; no software zero capture or mechanical homing occurs.

For an unloaded fixture whose current position and intended travel fit these
bounds, this is an example, not a universal travel range:

```text
servo inspect
servo errors
limits -0.1 0.6 10
profile 10 10
move 0.25 2
status
servo inspect
```

Repeating `move 0.25 2` requests the same absolute target. To cancel during a
move, enter `stop` on a separate line. Position setup configures PDOs and mode 1,
seeds the measured current position, and enables with Halt set before triggering
the requested target. Missing fresh initial position feedback prevents movement.

## Stop behavior

Stop/0000 always requests disable, including while busy, after an ESP32-only
restart or after a failed stop. The owner sends 0000 before status reads; mode,
demand and actual velocity do not block it. Stop cancels queued work and never
sends Halt or enable. It is handled when the current bounded transaction yields.

The owner checks `6041` for Switch on disabled, mask `004F → 0040`, for up to
one second. Even a missing write acknowledgement is followed by a live state
read. Failed reads, drive fault or timeout report disable unconfirmed; retry
`stop`. A disabled statusword does not prove standstill or mechanical brake lock.
There is no automatic restart. Completion of a position move uses this same path.

## Brake and explicit state steps

Use `servo brake-test` from the idle disabled, error-free bench state before
other motion in that ESP32 boot. It verifies mode 3 and zero target, writes
`60FF=0` immediately before `000F`, waits ten seconds, then writes `0000`.
Actual/demand velocity equality checks are bypassed. Mode, commanded target and
drive errors remain checked. Enabling the drive enables motor torque; physical
brake release and engagement must be observed.

Direct nonzero controlwords remain guarded. Direct 000F requires switched-on
state with no error, and either position mode 1 or zero-target mode 3. In mode 3,
it writes and verifies another zero velocity target before enabling. Position
mode can act on stored position/trajectory registers. A state-step command sends
only that step; it never follows automatically with another controlword.

## Parameters and serial capture

Typed tuning writes and sequential configuration snapshots exist in the
[motor API](docs/MOTOR_CORE.md); there is no generic parameter-write terminal
command or tuning catalogue yet. Nothing automatically saves parameters to EEPROM.

`tools/bench_serial.py --port PORT --log NEW_FILE.jsonl` records terminal traffic
on the computer without resetting/flashing or sending startup commands. Its
optional `--allow-brake-probe` or `--allow-state-steps` enables the corresponding
explicit commands in that helper's allowlist. Use the normal monitor for manual
moves. This computer-side capture is separate from the removed firmware SD logger.

Enter one command per line; the terminal does not support arrow-key history.
A leading FF byte or escape bytes cause rejection; retype a clean command. Save
complete output and describe observed motion, brake action, noise and temperature.
