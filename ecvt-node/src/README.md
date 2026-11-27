# Standalone firmware source map

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
