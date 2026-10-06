# SKF wheel-bearing RPM simulator

This bench firmware generates electrical signals for a separate sensor node; it does not transmit CAN. Read the [simulator overview](../README.md) and [hardware reference](../../docs/HARDWARE.md) first.

## Required boards and wiring

Operating the test needs an ESP32-C3 simulator board, a separate ESP32-C3 receiving board running `NodeEncoder`, USB data cables, and jumper wires. Building alone needs only PlatformIO. For CAN observation add the master and a properly connected CAN bus. For wheel serial observation, temporarily use the encoder profile's serial-test mode as described in [Testing](../../docs/TESTING.md).

| Simulator | Receiving encoder node |
|---|---|
| GPIO6 | GPIO6 (A) |
| GPIO7 | GPIO7 (B) |
| GND | GND |

The simulator drives 3.3 V push-pull signals. Share grounds; do not connect either input to more than 3.3 V or add a 5 V pull-up. These connections test digital capture, not the real sensor's power or ignition conditioning.

## Build, upload, and monitor

From the repository root, with PlatformIO installed, build without hardware:

```bash
pio run -d simulators/wheel-rpm -e WheelRPM_SKF_Simulator -t buildprog
```

Expect `SUCCESS`. This project defaults to upload/monitor when no target is given, so keep the explicit build target.

For a connected simulator board, identify its port with `pio device list`, then replace `PORT`:

```bash
pio run -d simulators/wheel-rpm -e WheelRPM_SKF_Simulator -t upload --upload-port PORT
pio device monitor --port PORT -b 115200
```

Expect the simulator startup message and console prompt. Type a command and press Enter. Monitor the receiving node separately to see measured values.

## Serial commands

| Command | Behavior |
|---|---|
| `rpm <value>` | Signed fixed speed, -3000 to +3000 RPM |
| `stop` | Stop output and cancel sweep |
| `sweep <start> <end> <step> <dwell_ms>` | Run a signed-speed sweep |
| `cancel` | End sweep while retaining current speed |
| `status` | Speed, direction, pins, sweep state |
| `help` | List commands |

## Expected observations

The generator starts at +500 RPM. It models SKF BMB-6202/032S2/UB108A with 32 pulses/revolution and 128 quadrature transitions/revolution. Quadrature means the two signals change in a sequence that encodes direction. Try `rpm 500`, then `rpm -500`: the receiving node should show similar magnitudes with opposite signs after its averaging window settles. `stop` should reach zero after the no-edge timeout. Confirm the sign against the configured channel order/direction convention rather than assuming a vehicle forward direction.

## Troubleshooting

If the simulator command works but the receiver is silent, check the correct receiving firmware, common ground, pins, recording/serial mode, and accepted input range. A simulator's `status` reports its configured output; it does not prove the receiver measured it. For signed bearing errors inspect channel order. For zero engine output inspect pulse intervals and capture overflow logs.

Next: [Testing](../../docs/TESTING.md), [Flashing](../../docs/FLASHING.md), or [Troubleshooting](../../docs/TROUBLESHOOTING.md). Return to the [simulator overview](../README.md).
