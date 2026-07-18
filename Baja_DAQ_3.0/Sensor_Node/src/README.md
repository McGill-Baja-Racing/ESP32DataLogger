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
    ├── bearing_encoder.c   Quadrature encoder
    ├── generic_adc.c       General GPIO1 voltage in millivolts
    └── engine_rpm.c        Documented placeholder for analog peak RPM
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

1. Create a driver in `src/sensors/` that defines one `sensor_t` descriptor.
2. Put hardware initialization in its `init` callback when initialization is
   required.
3. Put one non-blocking measurement in its `read` callback.
4. Set its CAN ID and sampling period in the descriptor.
5. Add the descriptor to the appropriate branch of `sensor_registry.c`.
6. Add the source file to `src/CMakeLists.txt`.
7. Add or update the PlatformIO build environment in `platformio.ini`.
8. Add the CAN ID to the master's accepted sensor list.
9. Document the value's unit and scaling, then build every affected profile.

Example descriptor:

```c
sensor_t example_sensor = {
    .name = "example",
    .can_id = 0x0BA,
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

`engine_rpm.c` is deliberately incomplete: it emits zero until tach signal
thresholds, hysteresis, pulses/revolution, filtering, and timeout are validated
on hardware. Do not replace those TODOs with guessed constants.

Keep `main.c` limited to startup and high-level wiring. Add a new module only
when it owns a distinct hardware interface, state, or substantial behavior.
