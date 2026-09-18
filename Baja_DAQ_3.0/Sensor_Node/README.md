# DAQ 3.0 Sensor Node

Firmware for the ESP32-C3 sensor nodes used by the Baja DAQ 3.0 system. Each
node reads a fixed set of sensors, timestamps samples using the master node's
clock, and sends the samples over a 1 Mbit/s CAN bus.

The firmware is intentionally small and build-time configured. The master
starts and stops sampling but does not configure sensor hardware at runtime.

## Supported builds

| PlatformIO environment | Node | Sensors |
|---|---:|---|
| `NodeBrake` | 1 | Front brake pressure on GPIO1 and rear brake pressure on GPIO2, both at 100 Hz |
| `NodeEncoder` | 4 | Signed bearing RPM on GPIO6/GPIO7 at 50 Hz |
| `NodeEngine` | 5 | Engine RPM on GPIO3 at 25 Hz |
| `NodeADC` | 6 | Generic ADC voltage on GPIO1 at 100 Hz |
| `NodeMPU` | 3 | MPU-6500/9250 acceleration and angular velocity on GPIO4/GPIO5 at 100 Hz |

All builds use CAN TX GPIO21 and RX GPIO20.

The CAN bitrate is set with `NODE_CAN_BITRATE` and defaults to 1 Mbit/s. START
and STOP commands are idempotent and acknowledgements are repeated so command
retries do not restart or stop the sampler more than once.

## Build

From this directory:

```bash
pio run -e NodeBrake
pio run -e NodeEncoder
pio run -e NodeEngine
pio run -e NodeADC
pio run -e NodeMPU
```

Upload and monitor one build with:

```bash
pio run -e NodeBrake -t upload
pio device monitor -b 115200
```

Replace `NodeBrake` with the profile for the node being programmed.

## How the firmware fits together

```text
Master START/STOP/time beacon
              |
              v
       can/can_node.c
          |       |
          |       `----> time/time_sync.c
          v
   sampler/sampler.c <---- sensors selected by sensor_registry.c
          |
          v
      sample queue
          |
          v
       CAN frames ------> Master logger
```

[src/main.c](src/main.c) is the composition root. It initializes the sampler
and CAN modules and connects their callbacks; it does not contain sensor or CAN
driver logic.

The module responsibilities and instructions for adding a sensor are in the
[source guide](src/README.md). The step-by-step procedures for adding sensors,
build configurations, and physical nodes are in
[Adding sensors and node configurations](docs/ADDING_SENSORS_AND_NODES.md).
CAN payloads and system behavior are described in the
[architecture document](docs/NODE_ARCHITECTURE.md).

## Runtime sequence

1. The node initializes the sensors selected by its build profile.
2. It starts CAN in the idle state and reports its boot state.
3. The master broadcasts clock beacons used to synchronize sample timestamps.
4. A START command enables the sampler.
5. Each due sensor is read and placed in the sample queue.
6. The send task packs the value and timestamp into an eight-byte CAN frame.
7. A STOP command disables sampling and clears pending samples.
8. If CAN enters bus-off, pending samples are cleared and the CAN module starts
   controller recovery while preserving the active/idle state.

## Data format

Sensor frames use eight little-endian bytes:

| Bytes | Contents |
|---|---|
| 0-3 | Signed 32-bit sensor value |
| 4-7 | Synchronized timestamp in milliseconds |

Node-state reports use CAN ID `0x0C0 + NODE_ID` for boot, start, stop, and CAN
recovery transitions. Normal sensor traffic provides node-liveness information
to the master while recording, so no heartbeat frames are sent. CAN controller
error-counter changes are printed locally even without reaching bus-off.

The high bit of each master-time beacon carries the Master's recording state.
After a node reboot, the next beacon restarts sampling automatically when the
Master is still recording; the lower 63 bits retain the master timestamp.

The engine RPM input measures rising-edge timing on GPIO3. Its one-spark-per-
revolution assumption, pulse rejection window, and stopped-engine timeout must
be validated against the conditioned ignition signal on the vehicle.

## MPU-6500 / MPU-9250 wiring

The `NodeMPU` profile uses the accelerometer and gyroscope over I2C. The
MPU-9250 magnetometer is not currently read.

| MPU breakout pin | ESP32-C3 sensor node | Purpose |
|---|---|---|
| `VCC` | `3V3` | 3.3 V power |
| `GND` | `GND` | Common ground |
| `SDA` | `GPIO4` | I2C data |
| `SCL` | `GPIO5` | I2C clock |
| `AD0` | `GND` or `3V3` | Select address `0x68` or `0x69`; both are detected |
| `NCS` / `CS` | `3V3` | Keep SPI disabled; many breakouts already pull this high |
| `INT`, `FSYNC` | Not connected | Not used by this driver |

Use external 2.2 kOhm to 4.7 kOhm pull-ups from SDA and SCL to 3.3 V unless
the breakout already provides them. Do not pull either I2C line to 5 V. The
driver enables the ESP32's weak internal pull-ups, but those are not a robust
substitute for external pull-ups on a vehicle harness.

The six CAN channels are `0x0B3` through `0x0B8`: acceleration X/Y/Z is
reported in milli-g and angular velocity X/Y/Z in milli-degrees per second.
The configured ranges are +/-8 g and +/-2000 degrees/second. With
`SENSOR_SERIAL_TEST=1` in the `NodeMPU` profile, the node starts without CAN
and prints readings to serial for wiring and axis tests.

For vehicle CAN operation, flash `NodeBrake`, `NodeMPU`, `NodeEncoder`,
and `NodeEngine` (plus `NodeADC` if fitted). These profiles all set
`SENSOR_SERIAL_TEST=0`. `NodeEngineBench` is serial-only and must not be
used for the vehicle CAN setup.

### Engine and wheel RPM across nodes

NodeEngine uses spark input GPIO3 only. NodeEncoder reads the bearing on
GPIO6/GPIO7 and sends RPM every 20 ms, averaged over 100 ms.
The master pairs each engine RPM with the latest wheel reading, provided it
is no more than 100 ms old, and logs engine_wheel_rpm (0x0BD) at the engine
timestamp. Missing or stale wheel data produces no pair. Raw bearing_rpm
(0x0B9) remains available. This is not a measurement between spark edges.

NodeEngine sends engine RPM (0x0BB) for valid 1,000–6,000 RPM intervals and
spark events (0x0BC) for every detected rising edge. After 100 ms without a
spark it sends zero engine RPM every 100 ms. Wheel speed remains independent.
Flash both NodeEngine and MasterStable for this configuration; NodeEncoder
requires no changes. NodeEngineBench prints engine readings without CAN.

Checks: `python3 tests/test_engine_capture.py` and
`python3 tests/test_rpm_pairing.py`.
