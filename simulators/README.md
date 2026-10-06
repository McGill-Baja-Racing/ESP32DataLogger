# RPM bench simulators

These projects let you test rotation inputs without running an engine or rotating a bearing. Read the [sensor introduction](../sensor-node/README.md) first. Building them needs no hardware; operating them needs a separate ESP32-C3 simulator board and a receiving sensor node.

| Project | Generated signal | Receiving firmware | Instructions |
|---|---|---|---|
| `engine-rpm/` | One positive pulse per revolution | `NodeEngine` or `NodeEngineBench` | [Engine simulator](engine-rpm/README.md) |
| `wheel-rpm/` | Two-channel quadrature, allowing speed and direction measurement | `NodeEncoder` | [Wheel simulator](wheel-rpm/README.md) |

Simulators generate electrical GPIO signals. They do not send CAN frames and do not replace sensor-node firmware. For end-to-end logging, the receiving node must send its measurements to a master.

Both simulator configurations default to upload and monitor targets. Always specify `-t buildprog` for a hardware-free build. Use the individual guides for explicit upload commands and serial controls.

Next: choose a simulator above, then use [Testing](../docs/TESTING.md) to validate the complete path. Return to the [project introduction](../README.md).
