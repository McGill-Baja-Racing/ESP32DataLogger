# Standalone firmware source map

The actuator is a servo motor with its own servo drive. The ESP32 communicates
with that drive over CANopen; it does not control the motor phases directly.
`motor/` and `servo/` refer to software responsibilities for this same actuator,
not two devices: operation ownership/sequencing versus drive protocol/feedback.
The names and some small helper files still need consolidation; this map describes
the current implementation, not a requirement to retain that split.

| Directory | Responsibility |
| --- | --- |
| `main.cpp` | Start servo CAN, the idle motor owner and the serial console |
| `can/` | Separate 500 kbit/s servo transport; node 1 SDO and four TPDOs only |
| `motor/` | Operation API, single owner, stop/cancellation, limits, position preparation and typed parameters |
| `servo/` | CANopen driver, synchronized feedback, disable and zero-velocity helpers |
| `experiments/` | Finite manual position runner and ten-second zero-velocity brake diagnostic |
| `console/`, `app/` | Serial parsing and mapping to motor requests |

`CMakeLists.txt` explicitly lists the firmware sources. There is no automatic
controller, DAQ/sensor acquisition, CSV replay, SD logger or legacy adapter.
Fixed large-travel presets and their reference fixtures are removed. Earlier
implementations are recoverable from Git history. The core remains independent
of the terminal so the future web UI can submit the same operations.
