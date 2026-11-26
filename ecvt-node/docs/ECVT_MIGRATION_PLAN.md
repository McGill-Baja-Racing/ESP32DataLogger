# ECVT migration plan — draft for review

Historical design/investigation record imported from ESP32_ECVT. For the
current standalone-first scope, see [Development](DEVELOPMENT.md); for executable
behavior, see [Terminal](../TERMINAL.md). Later bench results are in the
[November 25 report](bench/2026-11-25/README.md).

## Agreed direction

- Updated architecture: eCVT is a dedicated DAQ-connected node, not code running on the DAQ master. It receives existing sensor data on the DAQ CAN bus and controls the servo on a separate CAN bus. This supersedes the earlier master-integration proposal.
- User already has a suitable board model; no board purchase/model selection is needed. Confirm its pins/transceiver wiring during hardware setup.
- Required sensor acquisition/publication must remain available with DAQ recording stopped. Separate acquisition demand from recording state; exact lifecycle policy is still to be designed with the user.
- Central master logging is preferred if bandwidth and timing permit. Telemetry fields, rates, timestamps, and behavior on congestion remain design decisions; local SD is not selected.
- Initial debugging requires logging every motor command and every received servo feedback sample, plus the exact input snapshot used for each controller command. Do not silently downsample. Sequence numbers and loss detection are proposed so completeness can be assessed; transport reliability policy is not yet selected.
- Outside recording, only streams required by eCVT should remain active. Which engine source, brake channels, throttle source, and operating states require these streams still need definition. Recording should request the full normal DAQ set without overriding eCVT acquisition demand on STOP.
- Throttle input is tentatively a linear potentiometer sampled by a sensor node. Sensor-node assignment, wiring, calibration, units, update rate, and validity checks remain open.
- Both front and rear brake pressures are candidates for redundancy. No combination, disagreement, failure-detection, or brake-state threshold rule has been selected. They measure different hydraulic circuits; disagreement alone must not be assumed to identify a failed sensor.
- Sensor-loss handling is required and distinct from logging-loss handling. Required-input freshness limits, fault detection, actuator response, recovery/re-arm behavior, and behavior on master loss remain open.
- Confirmed logging-loss policy: continue control while required sensor inputs remain valid. Local fallback storage is desired for consideration, not yet a selected implementation. Propose bounded RAM buffering and optional SD spillover, with explicit gap reporting and later reconciliation; choose capacity, overflow policy, storage hardware, and replay behavior before implementation. Logging/storage must not block control. Master loss may also affect acquisition requests/time synchronization and must be evaluated separately from loss of logging alone.
- ESP32-P4 is the recommended chip, pending user selection and board pinout confirmation: it has three on-chip TWAI controllers; C3 and S3 each have one. Two independent buses require two controllers and two external CAN transceivers. C3/S3 would need an external controller for the second bus.
- Use the latest GitHub `main` of McGill-Baja-Racing/ESP32DataLogger, not its local checkout. On 2026-11-17 GitHub reported commit `2cf7a9849b78c406646dd7e6c6b3511334def109`. Refresh and record the SHA when creating the integration branch.
- Preserve the current working-tree `ECVT/src/main.cpp` and its supporting libraries, including uncommitted changes. This is the source baseline, not proof of hardware validation.
- Support full controller operation with sensor inputs and motor-only testing through terminal commands.
- Study existing tests and make their sequences repeatable without editing and reflashing for each run.
- Organize first, integrate second. The user owns behavioral and architectural decisions. Readability refactors must preserve behavior; fixes with behavior effects need a separate decision.
- One firmware will support terminal selection between automatic control and manual motor tests.
- For now, boot idle and wait for a terminal command before enabling motor operation.
- User approved stopping the active test/controller before changing manual/automatic mode and requiring an explicit start after switching. Do not implement an automatic restart on mode selection.
- BOM(B).xlsx (user-provided source at `/Users/remylaurendeau/Downloads/BOM(B).xlsx`) lists drive DSY-200C, motor DSY60-200L2B2M17S with brake, and a 50:1 gearbox. These are hardware references, not executable instructions; actual drive parameter values and brake wiring remain to be checked.
- Brake documentation now confirms drive output `FunOUT.11: BK` and state-related sequencing; separate brake power does not imply a separate ESP32 brake-control circuit. See `docs/DSY_BRAKE_CONTROL.md`. Use the drive-managed path for the proposed implementation; a dedicated independent CAN brake override has not yet been verified. A Halt by itself must not be assumed to engage the brake.
- Approved normal terminal position-test cancellation: controlled deceleration, then position holding; explicit restart required and no automatic move to zero or zero recapture. User also approved using the motor's mechanical holding brake to lock it after stopping. Implementation remains pending verification of drive stop settings, brake wiring/control, engage/release delays, standstill criteria, and torque-mode behavior. Do not use the holding brake as the normal decelerator or disable supporting servo torque before the brake's engagement interval has elapsed. On restart, establish holding torque before brake release and allow its release interval before motion. Do not claim confirmed mechanical brake engagement without a feedback source. This terminal-stop policy does not define the vehicle brake-input state or sensor-loss failsafe.
- The user remembers all existing tests working. Retain every test as reported working; source discrepancies require investigation, not automatic exclusion. A confirmed defective current variant can be excluded from the executable presets with its reason documented.

## Preserved baseline

2026-11-19 boot compatibility correction: user's ROM log identifies ECO7 and exactly matches the old pre-v3 bootloader load addresses. `ecvt-node` now uses board `esp32-p4_r3`, disables the old-silicon SDK selection, and compiles for revision 3.1–3.99. Kept the existing 2MB firmware layout rather than assuming the generic board's physical 16MB flash. A clean build passes, the linked bootloader uses `bootloader.rev3.ld`, its entry changes from `0x4ff29ed0` to `0x4ffac2c0`, and all six host checks pass. A full bootloader/app reflash and user-provided startup log are required to confirm the correction on hardware. No watchdog was disabled and no hardware was flashed during diagnosis.

Terminal checkpoint implemented after approval: idle boot; manual/automatic selection with explicit start; strict line parser; explicit per-boot movement limits; repeatable position-step, position-profile and step-response presets; cancellable legacy automatic bench loop; independent repeatable local recording. One motor owner and cancellation generations prevent concurrent/stale queued operations. CAN TX is bounded with persistent pending buffers and propagated errors. Actual position mode is read through 6061h; Halt confirmation uses synchronized, fresh status/velocity. Unconfirmed stops prevent new motion/mode changes. Physical BK lock remains unverified and is not automatically asserted. Validation: final ESP32-P4 firmware build passed; all six host checks passed, including the 76,403 calculation comparisons, parser/limits/admission/stop-confirmation checks, and actual C driver payload/error/buffer-lifetime tests. Original project files match the preserved source snapshot. No firmware was flashed. See `ecvt-node/TERMINAL.md` for exact behavior changes, command units, test coverage and outstanding hardware checks. The original projects remain unchanged.

First organization checkpoint implemented in `ecvt-node/` after user approval: controller, input/replay/ADC acquisition, CAN dispatch, servo, state machine, logging, and storage modules. Original projects remain unchanged. Firmware build passed with the baseline-resolved toolchain; four host migration checks pass, including 76,403 differential calculation cases, all 36 extracted function comparisons, and unchanged copied source/tool files. The only startup extraction is the SD mount helper, preserving the failure return path. This checkpoint deliberately retains automatic legacy startup; the agreed idle startup and terminal selection are the next behavioral change. Motor tests are catalogued in `ecvt-node/src/experiments/README.md`; no experiment runner or second bus is implemented yet.

Before firmware edits, sources (including untracked files) were saved to `/private/tmp/ecvt-baseline-pt7l2gdo/sources.tar.gz`, with per-file SHA-256 hashes, working-tree diff, and status alongside it. This temporary snapshot supplements the unchanged original sources; it is not a permanent archival location.

## Sequence and review points

Baseline build check on 2026-11-17: `ECVT` and `BAJA_ECVT` build successfully. `ECVT_Step_Reponse_Test` fails because `lroundf` is used without including `<math.h>`; no fix has been applied. Builds resolved the projects' moving stable platform URL to espressif32 55.3.313, ESP-IDF 5.5.6, and compiler toolchain 14.4.0+20261030. These results do not establish behavior on the original toolchain or hardware. Generated changes to all three tracked sdkconfig files were restored from the snapshot, and every snapshotted file was verified unchanged by SHA-256 after the build checks. General motor and step-response build logs are stored alongside the snapshot. The main build also reports legacy ADC deprecations and a board/config flash-size mismatch; configuration choices remain unchanged.

1. Preserve a source snapshot, including untracked source files; record hashes and working-tree diff. Establish build results for each existing project. Do not overwrite either working checkout.
2. Complete the experiment inventory below, including setup, wire payloads, units, timings, logging windows, termination, and differences between comments and executable code. Consult relevant history for superseded test variants; code alone cannot establish which tests succeeded physically. Ask the user to identify successful and obsolete variants.
3. Agree on an exact module/file map. Proposed responsibilities: application startup, DAQ input decoding/replay, controller calculations, state machine, separate DAQ/servo CAN channels, CANopen servo transport, logging, terminal parsing, experiment execution. Preserve existing local ADC RPM acquisition as legacy/bench functionality pending a decision on its future role.
4. Extract modules in small changes while retaining existing thresholds, formulas, timings, zeroing, and command scaling. Build and compare representative controller outputs and servo payloads with the preserved baseline. Record any pre-existing build failures separately.
5. Approve terminal command syntax, units, parameter limits, startup state, cancellation behavior, and ownership between controller and manual commands. Then implement commands and named experiment presets. Replace one-shot test task lifetimes with a repeatable runner only after agreeing how completion, stop, faults, and log sessions work.
6. Create an isolated DAQ development checkout/branch from freshly verified GitHub main. Establish its existing checks before integration. Add an independent `ecvt-node/` PlatformIO project; share the DAQ protocol definition rather than duplicating the master. Decide logging and health integration before adding master-side changes.
7. Approve DAQ input sources/conversions, per-bus bitrate and pins, recording-independent acquisition, actuation enable policy, and fault handling. DAQ input is received on one CAN channel; servo CANopen commands and feedback use the other. Do not forward raw frames between buses by default. Integrate inputs first, then calculated-command logging, then actual servo commands when authorized.
8. Agree on timing and load acceptance criteria; measure both CAN channels and control timing under the chosen logging workload. Validate independent bus fault behavior. Hardware validation and car deployment remain separate milestones.

## Existing experiment inventory (source review, not hardware validation)

| Source | Experiment | Behavior visible in current source |
|---|---|---|
| BAJA_ECVT `run_test1` | Drive state/enable | Maps controlword/statusword and enables operation; ends in a busy infinite loop. |
| BAJA_ECVT `run_test2` | Constant velocity | Commands 1000 for 3 s, then zero for 1 s, recording servo feedback. |
| BAJA_ECVT `run_test3` | Forward/stop/reverse | Commands 1000 forward and reverse using polarity bit 6, with intervening stop delays. |
| BAJA_ECVT `run_test4` | Velocity steps both directions | 500 then 2000 for 3 s each, stop, then corresponding reverse sequence. |
| BAJA_ECVT `run_test5` | Torque | Mode 4, raw target torque 10; waits 10 s and stays alive. Logging start and final disable are commented out. This is the current startup test. |
| BAJA_ECVT `run_test` | Position step | Calls target position 100; the driver multiplies by 10000. The comment says 10 rev, conflicting with executable code. Waits 12 s, then disables operation. |
| BAJA_ECVT `run_pos_test` | Position with motion profile | 75 rev, speed variable 2000 rpm, accel/decel variables 1666 rpm/s; waits for fresh target-reached feedback or 20 s timeout, then disables operation. |
| ECVT_Step_Reponse_Test `run_test` | Bidirectional position step | Commands +10 and -10 with speed argument 2000, waits 3 s each, records position, flushes log and disables operation. PDO mapping and payload length must be checked together before reproducing this test. |
| ECVT controller | Sensor-driven operation | Current control loop, CSV replay, local ADC RPM tasks, state transitions, directional profiles, and feedback logging must be documented as the baseline. |

Existing console menu code is present but the runner and console startup are disabled. Repeated tests cannot be enabled merely by uncommenting the menu: tests have permanent loops and logging shutdown behavior that need adaptation.

## Design questions to settle before implementation

- Agreed: one firmware switches modes through the terminal; boot idle pending a command; all tests are reported working. Still define switching, stopping, cancellation, and disconnection behavior.
- Confirm servo model/configuration and engineering units, including speed, acceleration, torque, position origin, and travel limits. Existing speed parameter names conflict with the division by 60 in serialization; preserve payloads until resolved.
- Approve command vocabulary and defaults. Candidate capabilities: status, enable/disable, position, velocity, torque, profile setup, named tests, log start/stop, and controller start/stop. These are proposals, not approved commands.
- For integration: determine engine RPM source/fallback, bearing-to-control-speed conversion, throttle source, brake pressure thresholds, stale-input rules, per-bus pins/bitrate, sensor lifecycle, and faults. Existing DAQ sensors provide inputs, but availability outside DAQ recording needs an explicit design; receiving CAN alone does not make idle sensors publish.

## Proposed final placement (not yet approved)

- DAQ `ecvt-node/`: independent controller firmware with DAQ CAN, servo CAN, terminal, and experiment modules.
- Motor experiments run in this same firmware through terminal commands; original projects remain reference sources during migration.
- DAQ `docs/ecvt/`: setup, command reference, calibration, and experiment catalogue.
- DAQ host tools/tests: decoding and meaningful controller/protocol verification.

No firmware changes, file moves, branch creation, or hardware commands have been made as part of drafting this plan. The snapshot and baseline build checks are preparatory work.
