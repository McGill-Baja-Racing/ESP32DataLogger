# Preserved motor experiments

The full reference implementations remain in the ESP32_ECVT source repository. Minimal immutable migration references are included under `tests/fixtures/migration-baseline/`. The three position presets are now available through the [terminal](../../TERMINAL.md); the remaining experiments stay catalogued pending their stopping behavior. None runs at boot.

| Proposed preset | Reference function | Existing sequence |
|---|---|---|
| `enable` | `BAJA_ECVT/src/main.c:run_test1` | Configure control/status PDOs and enable operation |
| `velocity` | `run_test2` in the same file | 1000 for 3 s, zero for 1 s |
| `reverse` | `run_test3` | 1000 forward 2.5 s, zero 0.8 s, reverse polarity at zero 0.2 s, 1000 reverse 2.5 s, zero 0.6 s |
| `velocity-steps` | `run_test4` | 500 and 2000 for 3 s each in each direction, with stop/polarity intervals |
| `torque` | `run_test5` | Profile torque mode, raw target 10, wait 10 s; no final disable; logging start commented out |
| `position-step` | `run_test` | Position argument 100, velocity 1000, acceleration 500, deceleration 100; wait 12 s, disable |
| `position-profile` | `run_pos_test` | Position 75, speed argument 2000, acceleration/deceleration 1666; wait for fresh target-reached or 20 s; disable after 250 ms |
| `step-response` | `ECVT_Step_Reponse_Test/src/main.c:run_test` | Positions +10/-10, speed argument 2000, 3 s each; position-only feedback log and final disable |

All tests are reported by the user to have worked on hardware. Values above describe the original executable source, not hardware validation of the new runner. Position terminal units retain the original 10000-count scaling; verify drive gearing. Preset completion/cancellation now sends 0000 and checks drive disable, replacing the earlier Halt-and-hold runner behavior and the original permanent-loop termination. The step-response runner maps both position and speed, matching its eight-byte command. These explicit differences are described in the terminal guide.

Before porting sequences, resolve:

- The position-step comment says 10 rev but its actual argument is 100; retain the discrepancy rather than quietly changing travel.
- Speed parameters named rev/s are serialized with division by 60; some callers label these values rpm.
- The step-response test maps only target position into RPDO2 but transmits eight bytes containing both position and speed. Establish what the drive accepted before changing the mapping.
- The step-response source uses `lroundf` without `<math.h>` and fails to build on the checked toolchain. The original is left intact.
- Permanent loops, one-shot logger shutdown, and disabled console startup prevent simply reusing these tasks for repeatable commands. Finite/cancellable sequences require an agreed motor action on completion/cancellation.

Servo mapping and profile settings must be part of each preset so repeating one test after another does not depend on an earlier test's setup. Final command syntax, parameter overrides and limits remain open.
