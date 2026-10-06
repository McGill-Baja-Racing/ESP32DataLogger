# Engine RPM simulator

This bench firmware generates electrical signals for a separate sensor node; it does not transmit CAN. Read the [simulator overview](../README.md) and [hardware reference](../../docs/HARDWARE.md) first.

## Required boards and wiring

Operating the test needs an ESP32-C3 simulator board, a separate ESP32-C3 receiving board running `NodeEngineBench`, USB data cables, and jumper wires. Building alone needs only PlatformIO. For CAN observation add the master and a properly connected CAN bus. For wheel serial observation, temporarily use the encoder profile's serial-test mode as described in [Testing](../../docs/TESTING.md).

| Simulator | Receiving engine node |
|---|---|
| GPIO8 | GPIO3 |
| GND | GND |

The simulator drives 3.3 V push-pull signals. Share grounds; do not connect either input to more than 3.3 V or add a 5 V pull-up. These connections test digital capture, not the real sensor's power or ignition conditioning.

## Build, upload, and monitor

From the repository root, with PlatformIO installed, build without hardware:

```bash
pio run -d simulators/engine-rpm -e EngineRPMSimulator -t buildprog
```

Expect `SUCCESS`. This project defaults to upload/monitor when no target is given, so keep the explicit build target.

For a connected simulator board, identify its port with `pio device list`, then replace `PORT`:

```bash
pio run -d simulators/engine-rpm -e EngineRPMSimulator -t upload --upload-port PORT
pio device monitor --port PORT -b 115200
```

Expect the simulator startup message and console prompt. Type a command and press Enter. Monitor the receiving node separately to see measured values.

## Serial commands

| Command | Behavior |
|---|---|
| `rpm <0-4000>` | Fixed speed; zero stops output |
| `stop` | Stop output and cancel sweep |
| `sweep` | 0 → 4000 → 0 RPM, 250 RPM steps |
| `status` | Target speed, pulse period, output pin, sweep state |
| `help` | List commands |

## Expected observations

The generator starts at 1000 RPM and outputs one rising edge per revolution with a 200 microsecond high pulse. Try `rpm 2000`: the receiving engine bench node should report approximately 2000 RPM after a valid pair of edges. `stop` should result in zero after the stopped timeout. The engine driver accepts only 1000–6000 RPM intervals, so simulator sweep speeds below 1000 are not valid nonzero RPM measurements. The simulator cannot exercise speeds above 4000.

## Troubleshooting

If the simulator command works but the receiver is silent, check the correct receiving firmware, common ground, pins, recording/serial mode, and accepted input range. A simulator's `status` reports its configured output; it does not prove the receiver measured it. For signed bearing errors inspect channel order. For zero engine output inspect pulse intervals and capture overflow logs.

Next: [Testing](../../docs/TESTING.md), [Flashing](../../docs/FLASHING.md), or [Troubleshooting](../../docs/TROUBLESHOOTING.md). Return to the [simulator overview](../README.md).
