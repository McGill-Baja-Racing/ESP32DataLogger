# Proposed organization and terminal interface

Historical design/investigation record imported from ESP32_ECVT. For the
current standalone-first scope, see [Development](DEVELOPMENT.md); for executable
behavior, see [Terminal](../TERMINAL.md). Later bench results are in the
[November 25 report](bench/2026-11-25/README.md).

Draft for user review. Runtime mode selection and idle startup are agreed; filenames, syntax, units, stopping behavior, and integration details below remain proposals.

Architecture update: this is an independent eCVT node, connected to existing DAQ sensors over one CAN bus and to the servo over a separate CAN bus. The master-hosted controller proposal is superseded. ESP32-P4 is recommended for two native CAN interfaces, pending user selection and board pinout confirmation. Each bus requires its own external transceiver.

Approved operating policy: stop the active operation before a manual/automatic mode switch, then require explicit start. Normal position-test cancellation decelerates, holds position and can engage the motor holding brake after standstill; do not automatically return to zero or change zero. Brake wiring/control and engage/release timing remain unverified, so hardware brake commands are not yet implemented. This policy is distinct from the vehicle brake-input state and unresolved sensor-loss response.

## First milestone: organize the existing firmware

Extract the controller in this repository before adapting it to DAQ services. Keep its ESP-IDF/PlatformIO configuration during extraction; toolchain upgrades are separate work. Keep the three existing project entry points during verification, then consolidate the experiment runner into the controller firmware. The final firmware will provide both automatic and manual modes without reflashing.

Proposed consolidated project:

```text
ecvt-node/
  platformio.ini
  CMakeLists.txt
  src/
    main.cpp                     startup and module wiring
    app/ecvt_app.cpp/.h          idle/manual/automatic ownership and command routing
    control/ecvt_controller.cpp/.h  current calculations and filtering
    control/ecvt_fsm.c/.h        existing controller states and entry actions
    inputs/sensor_inputs.cpp/.h  latest values and existing message decoding
    inputs/rpm_acquisition.cpp/.h  existing ADC pulse sampling, legacy/bench role TBD
    inputs/csv_replay.cpp/.h     existing SD replay
    can/can_transport.c/.h      two independent TWAI instances and dispatch
    can/daq_can.c/.h            DAQ messages and input routing
    can/servo_can.c/.h          separate servo bus transport
    servo/canopen_servo.c/.h    CANopen configuration, commands, feedback, zero offset
    console/serial_console.cpp/.h  parsing and help
    experiments/motor_tests.cpp/.h  finite, cancellable named sequences
    logger/ecvt_logger.cpp/.h   current log records and SD writer
    storage/sd_card.c/.h        SD mount
  tests/                       host checks for calculations and wire serialization
tools/ecvt/                    existing decoder and SD-copy tools
docs/ecvt/                     source guide, experiment catalogue, command reference
```

At DAQ integration, add `ecvt-node/` beside `master-node/` and `sensor-node/`, using the shared DAQ protocol. Reuse suitable source modules where helpful, but this node owns its own drivers and runtime resources. Decide local SD logging versus telemetry to the master separately. Do not assume a transparent bridge between buses. Runtime mode ownership is separate from the controller's existing Stopped/Running/Brake/Error states.

## Proposed command capabilities

| Example syntax | Purpose | Decision still needed |
|---|---|---|
| `help`, `status` | Show commands, mode, servo and input status | Exact fields |
| `mode manual`, `mode auto` | Select operating mode | Whether selecting auto also starts control |
| `controller start`, `controller stop` | Start/stop automatic control | Exact motor action on stop |
| `motor enable`, `motor disable` | Explicit drive state changes | Disable versus quick stop semantics |
| `motor position <value>` | Manual position target | Logical versus physical zero, position units and limits |
| `motor velocity <value>` | Manual velocity target | Units, sign versus polarity, limits |
| `motor torque <value>` | Manual torque target | Raw drive units versus engineering units, limits |
| `motor profile ...` | Set velocity/acceleration/deceleration | Units and supported parameters |
| `test list`, `test run <name>` | List/run preserved experiment presets | Names and parameter overrides |
| `test cancel` | Interrupt a running experiment | Motor action, acknowledgement, log completion |
| `log start`, `log stop` | Record without reflashing | Relation to test and automatic-control sessions |

Proposed preset names: `enable`, `velocity`, `reverse`, `velocity-steps`, `torque`, `position-step`, `position-profile`, `step-response`. Keep legacy values and wire payloads in the first presets. Distinguish reproducing a legacy sequence from changing its termination or logging behavior.

Do not infer engineering units from function names: speed parameters named revolutions/second are divided by 60 in serialization, while callers label some values rpm. Confirm the servo configuration before publishing terminal units. The step-response test maps only position into RPDO2 but transmits position plus speed; preserve this discrepancy in the catalogue until its behavior on the drive is established. A position-step comment also says 10 rev while code sends 100 rev.

## Implementation checkpoints

1. Baseline builds and source/experiment catalogue.
2. User agrees module layout; extraction preserves calculations and command bytes.
3. User agrees command syntax/units, mode transitions, and completion/cancellation behavior; implement repeatable runner and console.
4. Verify firmware builds plus representative calculation/serialization checks; hardware-test the presets with user-approved limits.
5. Integrate with freshly verified DAQ main after settling bus, input and lifecycle contracts.

No actuator commands are sent during repository preparation or build validation.
