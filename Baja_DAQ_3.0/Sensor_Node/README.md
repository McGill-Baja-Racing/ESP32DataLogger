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
| `NodeEncoder` | 4 | Bearing quadrature encoder on GPIO6/GPIO7 at 50 Hz |
| `NodeEngine` | 5 | Engine RPM placeholder on GPIO3 at 50 Hz |
| `NodeADC` | 6 | Generic ADC voltage on GPIO1 at 100 Hz |

All builds use CAN TX GPIO21 and RX GPIO20.

## Build

From this directory:

```bash
pio run -e NodeBrake
pio run -e NodeEncoder
pio run -e NodeEngine
pio run -e NodeADC
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
[source guide](src/README.md). CAN payloads and system behavior are described
in the [architecture document](docs/NODE_ARCHITECTURE.md).

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

Node-state reports use CAN ID `0x0C0 + NODE_ID`. Full periodic health reports
are intentionally not part of this bare-bones firmware.

The engine RPM stream currently reports zero intentionally. Its source file
documents the voltage-peak parameters that must be measured and implemented
before it can be treated as a real RPM signal.
