# Sensor Node source guide

This directory is organized by ownership. A module owns its internal state and
exposes a small public API through its header. Avoid accessing another module's
private variables or duplicating CAN IDs outside `protocol/app_protocol.h`.

## Directory map

```text
src/
├── main.c                  Application initialization and callback wiring
├── CMakeLists.txt          ESP-IDF source and include registration
├── protocol/
│   └── app_protocol.h      CAN IDs, node states, and transition reasons
├── can/
│   ├── can_node.h          Public CAN API and application callbacks
│   └── can_node.c          TWAI driver, RX dispatch, state TX, and recovery
├── time/
│   ├── time_sync.h         Synchronized timestamp API
│   └── time_sync.c         Master-to-local clock offset
├── sampler/
│   ├── sampler.h           Sampler lifecycle API
│   └── sampler.c           Scheduling, buffering, and sample transmission
└── sensors/
    ├── sensor.h            Common sensor descriptor interface
    ├── sensor_registry.c   Build-specific list of enabled sensors
    ├── adc_input.*         Shared ESP32-C3 ADC access
    ├── front_brake.c       Front pressure sensor
    ├── rear_brake.c        Rear pressure sensor
    ├── bearing_encoder.c   Quadrature bearing RPM
    ├── generic_adc.c       General GPIO1 voltage in millivolts
    ├── engine_rpm.c        Digital spark-edge timing and RPM conversion
    └── mpu6500.c           MPU-6500/9250 I2C accelerometer and gyroscope
```

## Module relationships

- `main.c` is allowed to know the major application modules and connect them.
- `can_node` owns the TWAI handle, receive queue, CAN tasks, and current reported
  node state. It notifies the application through callbacks.
- `sampler` owns the selected sensor list, schedule, active flag, and sample
  queue. It uses `time_sync` for timestamps and `can_node_send()` for output.
- `time_sync` owns the clock offset. Other modules do not read that offset
  directly.
- A sensor driver owns its hardware-specific state and conversion logic. The
  generic sampler only calls its `init` and `read` functions.

This creates one intentional dependency direction:

```text
main -> can_node
main -> sampler -> sensors
               -> time_sync
               -> can_node_send
```

## Adding a sensor

The complete procedures for adding a driver, adding a sensor to an existing
node, creating a new build configuration, updating the Master, and verifying
the result are in
[Adding sensors and node configurations](../docs/ADDING_SENSORS_AND_NODES.md).

The short checklist is:

1. Define a `sensor_t` descriptor in a driver under `src/sensors/`.
2. Add its CAN ID to the Sensor Node and Master protocol headers.
3. Add the source file to `src/CMakeLists.txt`.
4. Add the descriptor to the correct branch in `sensor_registry.c`.
5. Add a `platformio.ini` environment only when creating a new configuration.
6. Add the ID to the Master's accepted-sensor and sensor-to-node mappings.
7. Document its integer unit/scale, wiring, and sampling rate.
8. Build and bench-test every affected Sensor Node profile and the Master.

Example descriptor:

```c
sensor_t example_sensor = {
    .name = "example",
    .can_id = CAN_ID_EXAMPLE,
    .period_us = 20000,  // 50 Hz
    .init = init_example,
    .read = read_example,
    .context = &example_context,
};
```

The `read` callback returns the signed 32-bit value placed in bytes 0-3 of the
CAN payload. The sampler supplies the synchronized timestamp in bytes 4-7.

## Where changes belong

| Change | Location |
|---|---|
| CAN pins, node ID, selected build | `platformio.ini` |
| CAN command or state ID | `protocol/app_protocol.h` and the master protocol |
| CAN driver/recovery behavior | `can/can_node.c` |
| Timestamp calculation | `time/time_sync.c` |
| Scheduling or queue policy | `sampler/sampler.c` |
| Sensor calibration/conversion | Corresponding file under `sensors/` |
| Sensors included in a node | `sensors/sensor_registry.c` |

`engine_rpm.c` measures rising-edge timing on GPIO3 for one spark per
revolution. Its input conditioning, pulse rejection, and stopped-engine timeout
constants must be revalidated if the ignition hardware or expected engine-speed
range changes.
## Standalone serial sensor testing

Every fixed sensor environment defines this bench-test flag:

```ini
-D SENSOR_SERIAL_TEST=0
```

Leave it at `0` for normal CAN operation. Change it to `1` to make that
environment skip CAN initialization, start the sampler immediately, and print
each configured sensor independently to the serial console twice per second.

`NodeEngineBench` is a ready-to-use example with the flag set to `1`.

`engine_rpm.c` is deliberately incomplete: it emits zero until tach signal
thresholds, hysteresis, pulses/revolution, filtering, and timeout are validated
on hardware. Do not replace those TODOs with guessed constants.

Keep `main.c` limited to startup and high-level wiring. Add a new module only
when it owns a distinct hardware interface, state, or substantial behavior.

`mpu6500.c` uses one 14-byte burst read for each six-channel snapshot. The
first registry descriptor refreshes the shared snapshot and the remaining five
descriptors publish it, keeping all axes coherent. Change its pin, address,
range, bandwidth, and scale constants together when changing the hardware.
