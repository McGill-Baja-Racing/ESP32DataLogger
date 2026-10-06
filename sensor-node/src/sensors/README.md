# Sensor driver implementation

Read the [sensor source map](../README.md) and its concurrency vocabulary first. This guide explains the driver contract; use [Adding sensors](../../docs/ADDING_SENSORS_AND_NODES.md) for the extension procedure.

## Build selection and descriptors

`platformio.ini` selects a `NODE_FIXED_*_CONFIG` branch in `sensor_registry.c`. That branch copies exported `sensor_t` descriptors into the array returned to the sampler. An unselected driver is not initialized just because its source is compiled.

A descriptor in `sensor.h` supplies a name, CAN ID, period, optional initialization/session-start callbacks, read callback, and driver-owned context. The sampler owns `next_sample_us`.

For a **periodic** descriptor, use a strictly positive `period_us` and a `read(sensor_t *, int32_t *)` callback. Optional `init` runs at boot and optional `start` runs on an inactive-to-active transition. Read returns `ESP_OK` and writes the value only on success. Failed reads are skipped until the next scheduled attempt.

`period_us = 1,000,000 / Hz`: 100 Hz is 10,000 microseconds; 50 Hz is 20,000. Bounded reads matter because the periodic loop visits drivers sequentially.

## Event-driven engine exception

Engine descriptors have `period_us=0` and no `read` callback. This works because the engine build uses a dedicated sampler branch consuming `engine_rpm_next_event()`. Zero period is **not** a general event-driven driver API and must not be placed into the generic periodic loop.

The engine ISR captures edge timestamps/intervals into a 256-entry driver queue. The sampler emits spark events and valid RPM, or stopped-engine zeros. Serial bench output includes RPM only. CAN output reserves queue space for each spark/RPM event together; if space is insufficient, the event is lost and reported. Adding another event-driven sensor requires deliberate sampler/interface design, not just a new descriptor.

## Driver-owned hardware state

ADC support is shared by brake and generic voltage drivers. A new ADC configuration must participate in the sampler's ADC initialization condition. ADC reads use calibration when available, with an approximate fallback.

Bearing interrupts collect quadrature counts; periodic reads average speed. Protect shared counts and timestamps with critical sections. MPU axes share one 14-byte hardware snapshot: the first registry descriptor refreshes it, and the following five publish it. Preserve descriptor order and skip all axes when the burst fails.

Keep conversion, calibration, filtering, and hardware handles in drivers. Signed integer units/scales are defined by [Protocol](../../../docs/PROTOCOL.md); pins and hardware assumptions belong in [Hardware](../../../docs/HARDWARE.md).

Next: [Adding sensors/nodes](../../docs/ADDING_SENSORS_AND_NODES.md), [Node architecture](../../docs/NODE_ARCHITECTURE.md), and [Testing](../../../docs/TESTING.md).
