# Sensor nodes

ESP32-C3 sensor nodes turn electrical inputs into timestamped vehicle measurements and send them to the master over CAN. Read the [system overview](../docs/SYSTEM_OVERVIEW.md) if these concepts are new.

## One project, several board roles

A PlatformIO environment selects a fixed set of sensor drivers when firmware is compiled. Flash the environment matching the physical board. The master controls when the whole node samples; it does not choose sensor hardware at runtime.

| Environment | Node | Measurements |
|---|---:|---|
| `NodeBrake` | 1 | Front and rear brake pressure |
| `NodeMPU` | 3 | Acceleration and angular velocity on three axes |
| `NodeEncoder` | 4 | Signed gearbox bearing RPM |
| `NodeEngine` | 5 | Engine RPM and spark events |
| `NodeADC` | 6 | Optional generic voltage input |
| `NodeEngineBench` | 5 | Engine RPM printed locally; CAN disabled |

Most measurements are periodic: the scheduler reads them at a configured interval. Engine capture is event-driven: input edges are timestamped as they happen, and valid intervals produce RPM. It is not a fixed-rate polling sensor. The encoder is read periodically even though interrupts capture its rotation edges.

The bearing is inside the gearbox. Its raw RPM is recorded for later conversion to wheel and secondary RPM using the relevant drivetrain ratios; this firmware does not perform those conversions.

## Normal and bench operation

Vehicle profiles use `SENSOR_SERIAL_TEST=0`: initialize sensors, wait for master control, synchronize timestamps, then send samples over CAN. Repeated commands do not restart an already active sampler. A recording-state beacon lets a rebooted node rejoin.

With `SENSOR_SERIAL_TEST=1`, the node skips CAN, starts immediately after initialization, and prints each configured sensor at most twice per second. `NodeEngineBench` already selects this mode. Printed output is throttled; it does not represent the full acquisition rate. Serial bench timestamps use the local clock because no master beacon is received.

## First build

With PlatformIO installed, from the repository root, no hardware required:

```bash
pio run -d sensor-node -e NodeBrake -t buildprog
```

Expect `SUCCESS`. Follow [Flashing](../docs/FLASHING.md) when a board is available and [Hardware](../docs/HARDWARE.md) before connecting inputs.

Next: [Source map](src/README.md) → [Sensor driver guide](src/sensors/README.md) → [Adding sensors and nodes](docs/ADDING_SENSORS_AND_NODES.md). For runtime detail use [Node architecture](docs/NODE_ARCHITECTURE.md); for contracts use [Protocol](../docs/PROTOCOL.md).
