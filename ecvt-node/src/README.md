# Standalone drive-control source map

The ESP32 controls one servo motor through its servo drive over CANopen. It does
not control motor phases directly. The directories below describe software
responsibilities for that same actuator.

| Area | Files | Responsibility |
| --- | --- | --- |
| Startup | `main.cpp` | Start CAN, the idle drive owner and the serial console |
| Drive control | `drive/control.h`, `control.cpp` | Public requests/results/status, single operation owner, command dispatch and typed parameter access |
| Drive sequences | `drive/operations.h`, `operations.cpp` | Mode preparation, position move, brake/zero-torque diagnostics and unconditional disable |
| Internal policies | `drive/control_policy.h` | Motion bounds and exclusive admission/cancellation rules |
| Operation context | `drive/operation_context.h` | Small C-compatible cancellation/limits bridge between the C protocol client and C++ operation owner |
| CANopen access | `canopen/drive.h`, `drive.c` | SDO/NMT/PDO transactions, mapping and drive-unit serialization |
| Feedback | `canopen/feedback.h`, `feedback.cpp` | Shared latest TPDO values with synchronized reads and per-field timestamps |
| CAN transport | `can/transport.h`, `transport.cpp` | TWAI startup, ISR capture and routing for node 1 on TX5/RX4 at 500 kbit/s |
| Console | `console/console.h`, `commands.cpp`, `serial_console.cpp` | Line framing, parsing, mapping commands to drive requests, cached status and UART input |

Start with `console/commands.cpp` to follow a manual command. It submits a request
to `drive/control.cpp`; the owner runs the sequence in `drive/operations.cpp`,
which uses `canopen/drive.c` for transactions. Received frames travel through
`can/transport.cpp` to the SDO reply queue or the feedback cache. The future web
interface must use `drive/control.h` rather than call the protocol client directly.

The operation context stays separate because C and C++ callers share it without
exposing the entire application API. The feedback cache stays separate because
CAN RX and the owner share its data across tasks. These are useful boundaries;
individual mode changes and brake/stop functions no longer have separate files.

`CMakeLists.txt` explicitly lists eight implementation files. All source/header
files begin with their purpose; public contracts and non-obvious sequences have
brief comments. There is no automatic controller, DAQ acquisition, CSV replay, SD
logger or fixed movement preset. The separate `motor/`, `servo/`, `experiments/`
and `app/` directories have been replaced by the responsibilities above.

The current interface and drive behavior are described in
[terminal commands](../TERMINAL.md) and [the control API](../docs/MOTOR_CORE.md).
Historical import documents and bench evidence can refer to earlier file names;
Git history retains those implementations.
