# Bare-motor start/stop validation, 2026-11-25

Source: user-supplied [PlatformIO monitor transcript](brake-start-stop-monitor.txt),
with trailing whitespace normalized.
The trace was submitted after the user was instructed to upload `cb709f1` from
`ecvt/daq-readiness-20261125`. Messages match the simplified implementation,
but this excerpt contains no firmware boot hash or upload log to independently
identify the binary. No agent-run hardware command was used to collect it.

## Results established by the transcript

| Check | Observed result |
| --- | --- |
| Initial diagnosis | Ping returned mode 1; inspect returned 0250, operation disabled/no statusword fault; current error and error register zero, history count zero. |
| `stop` while disabled | 0000 sent and disabled state 0250 confirmed. |
| `servo cw 0x0000` while disabled | Routed to the shared disable path; 0000 sent and 0250 confirmed. |
| Brake-test start | Mode 3 and zero target verified; 0006/0007/000F transitions acknowledged and enabled state confirmed. Explicit zero target sent before 000F. |
| `stop` during enabled window | Received stop; test cleanup sent 0000 and confirmed 1250. Owner cancellation then sent another 0000 and confirmed 1250. Both writes were disables; no subsequent enable appears. |
| Cancellation return | Brake test returned ESP_ERR_INVALID_STATE because it was cancelled. This is its current cancellation result, not evidence of a drive fault. Follow-up current error/register/history all remained zero. |
| Uninterrupted brake test | Started again, remained in the enabled state, then automatically sent 0000 and confirmed 1650; result ESP_OK, no pending disable. |
| Final diagnosis | Status 1250, operation disabled/no fault, measured velocity zero; current error/register/history zero. DC-link reading 47900 unchanged at diagnostic checks. |

Enabled statuswords 0237/1237/1637/0637 all match the implementation's
Operation enabled mask (006F -> 0027). Disabled 0250/1250/1650 all match
Switch on disabled (004F -> 0040). Changes in other bits did not indicate a
statusword fault in this trace.

The first command was rejected because a leading FF byte preceded `status`.
Retyped commands worked. This reproduces the previously recorded first-line
serial issue; its origin is still unresolved. Frequent repeated state-confirmation
lines also clutter the transcript and interleave with local typing echo, but the
complete stop line was received and processed.

## User-confirmed physical result

After reviewing this transcript, the user confirmed that the test worked as
expected, with no vibration or overheating. This confirms their observation
of the requested brake release/reengagement cycle on the bare motor, alongside
the recorded drive enable/disable transitions and successful cancellation.
The bare-motor brake-cycle check is **passed for this tested setup**.
No calibrated timing or brake-coil voltage measurement was supplied.

## Limits and remaining checks

- No timestamps: exact ten-second duration and stop latency cannot be measured here.
- Brake release/reengagement and absence of vibration/overheating are user-reported physical observations. The firmware still has no independent brake-lock feedback; register values alone do not establish it.
- Settled position reads changed from -1 to -23 and finally -22 command counts. These snapshots do not describe the complete motion during enable.
- Direct 0000 was tested while already disabled; a separate direct-0000-from-enabled hardware check is not present. The shared stop path did disable from enabled state.
- MCU-only restart with the drive left enabled, automatic-mode stop, communication loss, other diagnostic modes, position moves and loaded operation were not tested here.
- This validates the observed bench sequence, not DAQ integration, a vehicle failsafe or tuning of the gearbox/load assembly.

The transcript supports progressing the observed start/stop checks from
host-only evidence to user-run bench evidence, while retaining the gaps above.

## Subsequent small-position-move trace

The user supplied a second [monitor transcript](position-move-monitor.txt),
with trailing whitespace normalized. It records manual mode, limits
-0.1..0.6 motor revolutions at maximum 10 rpm, and profile 10/10 rpm/s.

- Start position -22 counts; disabled, velocity zero, no current/error/history faults.
- `move 0.25 10` sent target 2500 counts and speed raw 1667. Target reached was
  reported at 2503; disable 0250 confirmed; inspection settled at 2495 counts.
- `move 0.5 2` from 2495 counts sent target 5000 and speed raw 333. Target
  reached was reported at 5003 and disable was confirmed.
- Further `move 0.5 2` commands started near 5000 counts, so they requested
  little displacement rather than another half revolution. They completed
  and disabled. These are absolute position commands.
- No complete `Received: stop` command occurs during these moves. A locally
  echoed `s` does not establish a submitted stop. Moving cancellation is
  therefore **not yet demonstrated** by this trace.
- A `move -0.5 2` line and a subsequent inspect line contained arrow-key
  escape sequences and were rejected, with no actuation from those lines.
  A correctly typed -0.5 target would also lie outside the configured -0.1
  lower bound; do not widen bounds merely to repeat this attempted command.
- Final status is idle, busy=0, no pending disable, ESP_OK; feedback shows
  position 4998 and velocity -1. Current error/register/history remain zero.

Software target/feedback and automatic disable checks passed for the recorded
small moves. Physical travel/direction/smoothness and a completed stop command
during motion still need user observations/evidence. No full motion waveform
or independently timed stop latency is available. A distinct in-range target
(e.g. 0 from the observed position near 0.5, at 2 rpm) provides time to submit
stop; repeating the current absolute target does not.

## Moving cancellation follow-up

The user subsequently supplied an inline serial trace with these results:

| Observation | Value |
| --- | --- |
| Initial status/position/velocity | 0250, disabled/no fault; 4999 counts; velocity 0 |
| Limits/profile | -0.1..0.6 motor rev, maximum 10 rpm; 10/10 rpm/s |
| Accepted move | `move 0 2`: target 0 counts, profile speed raw 333 |
| Enable transitions | 0231 -> 0233 -> 0237, followed by setpoint words 002F/003F |
| Cancellation | Full `Received: stop`, then 6040=0000; disabled state 1250 confirmed before any target-reached report |
| Follow-up position/velocity | 4329 counts, velocity +1 on inspect and -1 in subsequent status |
| Final software state | manual, idle, busy=0, error ESP_OK; no pending disable |
| Errors | Current code 0000, error register 00, history count 0; DC-link raw 47800 |

This passes the logged moving-cancellation check: the shaft position changed
toward the commanded zero, then the drive reported disabled while position
remained short of that target. Small nonzero measured velocity did not block
disable. No automatic restart is present in the supplied follow-up.
Physical stop latency and long-term standstill still require user observation;
the trace has no complete timestamp coverage for those claims.

Two malformed profile lines were rejected (one concatenated/duplicated command
and one missing the initial `pr`). A later correctly typed `profile 10 10` was
accepted before movement. The rejected lines did not change the motion profile.

The earlier section's moving-cancellation gap is superseded by this follow-up.
Direct-0000-from-enabled, MCU-only restart recovery and other untested modes/fault
cases remain separate checks; no loaded-system or DAQ readiness is inferred.

## Physical confirmation of the tested sequence

After the moving-cancellation follow-up, the user confirmed that all of the
performed tests worked as expected. This confirms the requested observations
for completed small position moves and prompt stop with no unintended restart
or vibration, in addition to their earlier brake-cycle/no-overheating report.
The **basic bare-motor start, small-position-move, brake-cycle and stop checkpoint
is passed for the tested setup**.

This is user-observed bench evidence, not an independently timed measurement.
The separate unperformed restart, direct-0000-from-enabled, other modes/faults,
loaded-system and DAQ integration checks listed above remain open.
