# Source map

`main.cpp` initializes receive/storage services, the motor owner and terminal without starting motion. Read it first, then the module for the behavior being examined.

| Module | Responsibility |
|---|---|
| `app/ecvt_app.cpp` | Single motor owner, explicit mode/start, preparation, cancellable operations and stop confirmation |
| `console/` | Bounded line input, strict command parser and help |
| `control/ecvt_controller.cpp` | Cancellable synchronous controller loop, original RPM calculations/filter/transitions and guarded commands |
| `control/ecvt_fsm.c` | Original state transitions, enable/stop actions, zero capture |
| `inputs/sensor_inputs.cpp` | Original scalar/frame decoding and synthetic input enqueueing |
| `inputs/rpm_acquisition.cpp` | ADC pulse sampling and stopped-RPM detection |
| `inputs/csv_replay.cpp` | Existing optional CSV fields, input replay and timing |
| `inputs/can_sensor_map.c`, `.csv`, `.h`, `_entries.inc` | Legacy sensor/servo CAN ID definitions and lookup |
| `can/can_transport.cpp` | Single legacy TWAI RX callback, queue dispatch, latest inputs and servo feedback capture |
| `servo/drive_state.cpp` | Unconditional 6040=0000 disable, followed by status confirmation; no mode/velocity prerequisite |
| `servo/ECVT_motor.c`, `servo_feedback.cpp` | CANopen serialization, bounded TX storage/error propagation, motion guards, synchronized feedback |
| `logger/ecvt_logger.cpp` | Original 44-byte records, repeatable sessions, persistent writer and counted losses |
| `storage/sd_card.cpp` | Original SD mount configuration and failure path |
| `experiments/motor_tests.cpp`, `README.md` | Repeatable/cancellable position presets and catalogue of remaining tests |

Each extracted C++ module has a matching header. `app/runtime_internal.h` contains the migration's common types/configuration and declarations for cross-module state; each mutable object is defined once in its owning module. `app/modules_internal.h` aggregates private module interfaces. These are internal migration interfaces, not APIs to expose to DAQ or terminal clients. Existing C libraries retain their own public headers and C linkage.

Legacy sensor values remain shared; complete consistent controller snapshots are a later integration change. Servo feedback for confirmation uses a critical section, motor operations have one owner and generation-based cancellation, and logging admission is separate from SD I/O. ADC acquisition and CSV replay start only for automatic bench operation and stop cooperatively.

See [terminal behavior](../TERMINAL.md) for explicit changes from the baseline. Two-bus routing, master telemetry and physical brake integration follow after the standalone behavior is established. The old periodic `save_task` helper is retained as a reference but is not started; the writer flushes each block and closes on explicit session stop.

Terminal stop and normal runner completion now send `6040=0000` before checking live drive state. No mode, speed or local enable-memory check blocks the write. See the terminal guide for confirmation limits and pending-disable retry behavior.
