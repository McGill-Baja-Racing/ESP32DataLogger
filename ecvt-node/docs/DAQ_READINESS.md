# eCVT node readiness for DAQ integration

Current direction (2026-11-26): the node is now colocated in DAQ on
`BajaRemy/ecvt-standalone`. Runtime integration is deliberately deferred until
the standalone manual-control/tuning interface and test-script workflow are ready.
See [the development sequence](DEVELOPMENT.md) for current priorities.
The older integration-first work order below is retained as historical context.

Audit date: 2026-11-25. Source checkpoint: `d2f959e` on
`ecvt/daq-readiness-20261125` in `ESP32_ECVT`.
This checklist describes the current implementation; it does not certify vehicle readiness.

## Destination and preserved baseline

The destination is an independent `ecvt-node/` PlatformIO project beside
`master-node/` and `sensor-node/` in `McGill-Baja-Racing/ESP32DataLogger`.
DAQ inputs and servo CANopen use separate CAN controllers/transceivers.
The eCVT controller does not run on the master and raw frames are not bridged between buses.

Fresh upstream DAQ `main` inspected on the audit date:
`5fa2917e4d311e9a419ffd37b93ba92420df3806`.
Read its `protocol/app_protocol.h`, `docs/PROTOCOL.md`, `CONTRIBUTING.md`, and
`docs/TESTING.md` before implementing integration. Refresh the base if development resumes later.
The existing local DAQ testing checkout is not the integration baseline.
A fresh local branch, `BajaRemy/ecvt-node-integration`, is prepared at that SHA
in `/private/tmp/ecvt-daq-integration-20261125`. It currently contains unmodified
DAQ main; no eCVT source or protocol change has been imported or published.
That checkout is temporary; move/recreate it in a durable worktree before migration.

The source checkpoint preserves the previously uncommitted original controller changes,
extracted node, migration plans, vendor references, and recorded bench evidence.
Generated `.pio/` products and macOS `.DS_Store` changes are not part of the checkpoint.
An additional full file/hash/patch snapshot was saved at
`/private/tmp/ecvt-checkpoint-20261125-msj26a9n`; this temporary directory is supplementary,
not the durable source archive.

The earlier [migration plan](ECVT_MIGRATION_PLAN.md) and
[CAN budget](ECVT_CAN_BUDGET.md) remain design context.
Their checkpoint descriptions and November 20 hardware findings are historical;
use this audit and the current terminal guide for current implementation status.

## Functionality and evidence

“Host checked” below means selected code paths are tested without a drive.
It does not establish FreeRTOS scheduling, physical stopping, brake operation,
units, load response, or CAN timing. “User reported” is distinct from an archived measurement.

| Capability | Current implementation | Evidence and remaining acceptance |
| --- | --- | --- |
| Modular node | Startup, owner task, console, controller/FSM, inputs, CAN, servo, experiments, logger, storage | Build passes. Calculation/extraction comparisons pass. Runtime state still spans shared internal headers; audit interfaces before DAQ adapters. |
| Idle startup | No servo setup/enable/motion at boot; manual mode default | Startup/parser checks and prior boot traces. An idle ESP32 does not establish a disabled drive after an MCU-only restart. |
| Manual/automatic ownership | One motor owner, busy admission, cancellation generations; explicit start after mode selection | Host checked. Stop/0000 bypass busy/mode admission; hardware confirmation remains pending. |
| Live diagnostics | Ping, inspect, errors/history, typed object reads | Prior drive replies and user logs. Read-only commands do not establish physical brake state. |
| Explicit controlword steps | 0000 routes directly to cancellation/disable; other individual state steps retain start checks. Mode-3 000F writes/readbacks zero target first. | Prior live transitions; new stop/0000 and direct mode-3 enable paths host checked. Powered validation of the updated firmware is pending. |
| Stop/cancellation | Always sends 0000 before reading status, then checks Switch on disabled; no mode/velocity/local-enable prerequisites. Normal runner completion also disables. | Production routing/finish/disable host harness covers MCU-only restart, busy/automatic operation, fault, failed acknowledgement/read, timeout and retries. No updated hardware validation yet. |
| Brake test | Mode 3, target 0, 0006 -> 0007 -> explicit target 0/readback -> 000F, 10 s, then 0000 | Host sequence/cleanup checks. User logs confirm transitions but enabled wait previously aborted on a combined mode/target/demand/error check; original message did not identify which field failed. The current bench sequence checks zero commanded target, mode and errors, not demand or actual velocity. User subsequently confirmed the bare-motor release/reengagement cycle worked as expected without vibration or overheating; see the bench report. |
| Disable retry | Shared disable helper with pending-disable latch; stop/0000/brake-restore always attempt 0000 even without remembered motion | Host checked. Live status can confirm disabled despite a lost write acknowledgement; failed confirmation stays visible. No reliance on RAM alone after reboot. |
| Position operation | Explicit limits/profile; move; position-step, position-profile, step-response; bounded three-revolution and hold diagnostics | Build/selected host checks. Earlier hardware trials faulted. Revalidate with working tuning and approved travel; legacy presets have large targets and are not universal safe defaults. |
| Other motor experiments | Original enable, velocity, reverse, velocity-steps, torque sequences catalogued | Full repeatable velocity/torque presets not yet exposed. A zero-torque brake diagnostic exists; it is not a general torque preset. Decide retained experiments and their completion/cancel behavior before porting. |
| Automatic eCVT control | Extracted legacy calculations, filtering and FSM, with manual start | 76,403 differential calculation cases pass. Complete live input snapshot/freshness, state transitions, physical zero/travel and cancellation validation outstanding. Preserve formulas during integration; separate deliberate behavior changes. |
| Bench inputs | Legacy CAN map, ADC RPM and optional CSV replay | Legacy compatibility only. These are not current DAQ signal definitions. Keep bench sources explicitly selected; prevent replay/synthetic input from impersonating live vehicle data. |
| Input consistency/freshness | Legacy sensor scalars remain shared; servo feedback is synchronized | Coherent timestamped sensor snapshots and required-input age/validity checks remain to implement. |
| Local logging | Repeatable SD sessions, binary records, loss counters, decoder and copy tool | Build/host checks do not establish full provenance or hardware SD reliability. Earlier SD mounting failed. Complete manual/automatic command and received-feedback coverage audit. |
| DAQ input bus | Not implemented; current transport is single-bus legacy 500 kbit/s | Add separate DAQ 1 Mbit/s adapter using shared protocol, without altering servo bus timing or forwarding raw frames. Confirm pins/transceivers. |
| DAQ telemetry/master logging | Not implemented | Agree fields, IDs, timing, fragmentation, sequence/loss detection and congestion behavior; update master/export/decoder together. Logging loss alone must not halt otherwise-valid control. |
| Acquisition outside recording | Not implemented | Current DAQ sensors follow START/STOP and recording beacons. Add an explicit acquisition-demand design so stopping recording does not remove required eCVT inputs. |
| Faults/recovery | Some drive feedback, transport, timeout and stop guards exist | Define required input loss, DAQ/servo bus loss, master loss, drive fault, restart and explicit re-arm behavior. A drive fault is not to be bypassed by tuning/protection changes. |

## Motor configuration evidence

The [pre-reset parameter export](commissioning/dsy-c-before-inertia-reset-20261124.txt)
was supplied by the user and read as data. The repository copy normalizes line
endings and trailing whitespace only; parameter values and other bytes are unchanged.
Original export SHA-256: `786cc22f4d054db8133724ee170cb45e68da8fa9fd3be19c860243e205385a2b`.
It is a historical configuration,
**not a configuration to write back to the drive**.
It records `P00.05=3000` (30.00 inertia ratio per manual scaling), rigidity 15,
speed-loop gain 36.0 Hz and integral time 20.83 ms.

The user reports that resetting inertia stopped the bare motor's vibration and
that it then responded well in the manufacturer software. They believe only
inertia was reset. No post-reset export or recorded comparison is available yet;
the new inertia value and other changed settings remain unknown.
Do not declare the old faults solely a brake/encoder failure, or reuse loaded
gearbox tuning for the bare motor. Preserve a read-back export of the working
configuration before additional tuning. Final loaded-system tuning is separate
from protocol migration and must use the intended mechanical assembly.

## Work order and acceptance gates

- [x] G0: preserve source and establish standalone software baseline.
- [x] G1: record the functionality inventory, known defects and integration decisions.
- [x] S1 software: explicit 0000 bypasses state/busy/mode checks and shares unconditional disable/status confirmation. Missing acknowledgement is printed; failed confirmation remains visible; no mechanical-lock claim. Hardware validation pending.
- [x] S2 software: stop writes 0000 before reading live state, regardless of remembered enable, operating mode or velocity. Production-path host tests cover MCU restart, busy/automatic state, fault, timeout and acknowledgement/read failures with no enable/Halt writes. Hardware validation pending.
- [ ] G2: finish approved standalone behavior and diagnostic/production command boundaries. Host-test S1/S2 and required runner behavior, then perform separately authorized hardware checks with recorded results.
- [ ] G3: prepare a fresh DAQ feature branch and establish existing DAQ host/build baselines; keep unrelated local checkouts intact.
- [ ] G4: import the independent node and portable tests/docs; preserve controller behavior. Remove test dependencies on absent sibling legacy projects by including minimal immutable baseline fixtures/provenance, rather than copying old firmwares into DAQ runtime code.
- [ ] G5: implement and test separate buses and DAQ decoding; first observe inputs without servo actuation, then compute/log commands without transmitting movement.
- [ ] G6: implement acquisition demand, telemetry, loss reporting, input freshness/failsafe and re-arm behavior under agreed contracts.
- [ ] G7: validate hardware integration, bus load/latency, complete logging, repeated mode/test runs, faults/reboots and intended mechanical load.
- [ ] G8: merge only after required repository checks, independent validation and review. Do not merge during preparation or infer vehicle deployment permission from a merge.

## Decisions required before DAQ implementation

| Decision | Required definition |
| --- | --- |
| Inputs | Engine source/fallback, bearing-to-controller-speed conversion, throttle node/calibration, brake-pressure-to-brake-state rule |
| Timing | Input rates, local receive-age thresholds, coherent snapshot rules and treatment of rebooted source/master timestamps |
| Acquisition | Ownership of eCVT demand vs recording/live-view demand; behavior on each consumer stopping or disappearing |
| Actuation | Automatic start/re-arm, zero/homing reference, travel/speed bounds and mode-specific completion/stop |
| Faults | Actuator action for each lost required input, bus or master; explicit recovery conditions |
| Logging | Full command/input/received-feedback record contract, node/signal IDs, health semantics, timestamping, congestion policy and any fallback storage |
| Hardware | Separate bus pins/transceivers, termination and bitrates; controller board revision and intentional flash/toolchain defaults |
| Experiments | Required legacy presets; which diagnostics stay bench-only; approved units, limits and completion/cancel behavior |

Do not equate DAQ START/STOP or its recording beacon with motor enable/disable.
Do not allocate new IDs without checking control/state/health ranges in the shared header.
The current DAQ bearing signal is raw bearing RPM, not wheel RPM. Its AiM wire
input at 0x500 is distinct from the master-only normalized log ID 0x703.
DAQ sensor payloads are signed integer values plus synchronized millisecond timestamps;
the eCVT legacy float/scalar map cannot be substituted by changing CAN IDs alone.

## Software validation recorded on 2026-11-25

At checkpoint `d2f959e`, from `ecvt-node/`:

```sh
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk python3 -m unittest discover -s tests -v
/Users/remylaurendeau/.platformio/penv/bin/pio run -e esp32-p4
```

Result: all 9 host tests passed, including 76,403 calculation comparisons;
firmware build succeeded. Host checks cover selected runner/parser/transport
and migration behavior, not the complete owner-task state machine or physical operation.
No upload or actuator command was performed for this audit.
The moving platform URL, SDK defaults, flash-layout discrepancy and ADC deprecations
remain known separate configuration work; this checkpoint does not silently update them.

Fresh DAQ baseline checks at `5fa2917`:

```sh
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk python3 -m unittest discover -s master-node/tests -v
```

Result: all 35 unittest cases passed; CSV exporter/paired CSV scripts also
reported success during discovery. Full DAQ firmware build matrix has not been
run in the fresh checkout. G3 remains incomplete until affected baseline builds
are recorded. Existing DAQ firmware and its original local checkout were unchanged.

## Bench stop simplification, 2026-11-25

The user selected a simpler bench policy: zero commanded velocity and the normal
0006/0007/000F enable sequence, then unconditional 0000 for stop. This deliberately
replaces the previous generic position-only Halt/standstill gate and Halt-and-hold
completion. The implementation shares a servo-layer disable helper; brake-test
cleanup and the owner use the same write-first, status-afterward operation.
`stop`, `controller stop`, `test cancel` and `servo cw 0x0000` bypass admission
while busy or in automatic mode. The command cancels old queued operations.
The obsolete exact-zero StopConfirmation policy was removed.

Demand-velocity equality is no longer required by velocity-zero preparation or
the ten-second brake test. Zero target readback, mode verification, drive fault
reporting, command parsing and motion bounds remain. Direct mode-3 000F also
writes zero target before enabling; the explicitly supervised mode-1 state step
retains its existing position-mode behavior.

Additional host verification: `tests/test_stop.py` compiles actual production
submission, finish and single-controlword handler code with the real disable
and brake modules. It verifies that stop writes before its first read, never
sends an enable/Halt word, works after reboot and while busy/automatic, retries
a failed disable, handles a missing acknowledgement via live state, reports
read/fault/timeout uncertainty, and sends zero target before a direct mode-3 enable.
No enabled hardware test or flash is part of this change.

Validation of the simplified stop implementation: all 10 host tests passed
(including 76,403 calculation comparisons); the `esp32-p4` firmware build
succeeded. Diff whitespace checks passed. Hardware validation of this revision
remains pending.

## User-run start/stop trace, 2026-11-25

See the [bench report and submitted transcript](bench/2026-11-25/README.md).
The trace confirms stop and explicit 0000 while disabled, the shared stop path
from the brake test's enabled state, and uninterrupted automatic test cleanup.
Final status confirms disabled; current error/register/history remain zero.
The cancellation's ESP_ERR_INVALID_STATE is the runner's cancellation result.
The user subsequently confirmed the physical brake cycle worked as expected,
with no vibration or overheating. The bare-motor brake-cycle check is passed
for this tested setup. Exact timing and the remaining restart/mode/fault cases
remain unverified. No vehicle or DAQ
integration acceptance gate is marked complete from this bench trace alone.

Subsequent [small position moves](bench/2026-11-25/README.md#subsequent-small-position-move-trace)
reached 0.25 and 0.5 motor-revolution targets and automatically disabled with
no recorded drive faults. The repeated 0.5 commands requested the same
absolute target. No full stop command was received during a moving segment;
moving cancellation and physical travel/smoothness remain pending.

A subsequent user-supplied inline trace completed the moving-cancellation check:
`move 0 2` from 4999 counts was interrupted by an accepted `stop`; 0000 was
sent, status 1250 confirmed disabled, and follow-up position was 4329 rather
than target 0. Current error/register/history remained zero, state returned
to idle/busy=0, with no pending disable. This supersedes the earlier missing
stop-command evidence. Physical stop latency/stability and the remaining
restart/mode/fault cases are not established by these snapshots.

The user then confirmed that all performed tests physically worked as expected,
including the completed small moves and moving stop. The basic bare-motor
start/move/brake/stop checkpoint is passed for this setup. This is sufficient
to proceed with software migration and DAQ input integration, not vehicle or
loaded-system acceptance. Remaining restart/mode/fault and integration gates
are unchanged.
