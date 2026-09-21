# Adding sensors and node configurations

This guide covers the common ways to extend a DAQ 3.0 Sensor Node:

- add a new sensor to an existing node configuration;
- create a new configuration from existing sensor drivers;
- create a new physical node with one or more new sensors; or
- change a sensor's pins, rate, conversion, or hardware setup.

## The pattern to understand first

Sensors are selected at **build time**, not individually enabled at runtime.
Each PlatformIO environment defines one `NODE_FIXED_*_CONFIG` macro. That macro
selects a branch in `src/sensors/sensor_registry.c`, and that branch returns the
only sensor descriptors the sampler can see.

At runtime, START and STOP apply to that complete list:

```text
platformio.ini environment
        |
        | defines NODE_FIXED_*_CONFIG
        v
sensor_registry.c
        |
        | returns sensor_t descriptors
        v
sampler_init()
        |
        | calls optional descriptor.init functions once
        v
CAN START or SENSOR_SERIAL_TEST=1
        |
        v
sampler_start()
        |
        | calls optional descriptor.start functions
        v
sample_task()
        |
        | when each descriptor is due
        v
descriptor.read(descriptor)
```

There is no runtime `sensor_enabled` flag. A driver that is not placed in the
selected registry branch is never initialized, started, or read by the sampler.

## Files involved

| File | Change it when... |
|---|---|
| `src/sensors/<sensor>.c` | Adding or changing hardware access and value conversion |
| `src/sensors/sensor.h` | Changing the interface shared by every sensor; rarely necessary |
| `src/sensors/sensor_registry.c` | Choosing which sensor descriptors belong to each build |
| `src/protocol/app_protocol.h` | Assigning a new sensor CAN ID |
| `src/CMakeLists.txt` | Adding a new `.c` source file to the firmware build |
| `platformio.ini` | Adding a build environment, node ID, configuration macro, or test mode |
| `../Master_Node/src/protocol/app_protocol.h` | Allowing the Master to recognize the ID and map it to its node |
| Master logging/metadata files | Giving the new signal a name, unit, or output column when required |

Do not add sensor-specific decisions to `main.c` or `sampler.c`. Those modules
are deliberately generic.

## Workflow A: add a sensor to an existing configuration

Use this when a physical node already exists and should report an additional
measurement.

### 1. Choose a CAN ID and data representation

Add a unique ID to `src/protocol/app_protocol.h`:

```c
#define CAN_ID_STEERING_ANGLE 0x0BC
```

Before choosing it, check both Sensor Node and Master protocol headers for
collisions. Decide and document the returned value's unit and scale, for
example degrees, millivolts, RPM, or milli-degrees.

Every `read` callback returns `esp_err_t` and writes an `int32_t` through its
output pointer on `ESP_OK`. Failed reads are skipped and retried at the next
scheduled period. The sampler transmits successful values in
CAN payload bytes 0-3 and puts the synchronized millisecond timestamp in bytes
4-7. Both fields are little-endian.

### 2. Create the sensor driver

Create `src/sensors/steering_angle.c`. A typical polling driver looks like:

```c
#include "sensor.h"

#include "protocol/app_protocol.h"

typedef struct {
    /* Driver-owned handles, state, calibration, or counters. */
} steering_context_t;

static steering_context_t steering;

static esp_err_t init_steering(sensor_t *sensor)
{
    steering_context_t *context = sensor->context;
    /* Configure the hardware once. */
    (void)context;
    return ESP_OK;
}

static void start_steering(sensor_t *sensor)
{
    steering_context_t *context = sensor->context;
    /* Optional: clear counters or filters at every recording START. */
    (void)context;
}

static esp_err_t read_steering(sensor_t *sensor, int32_t *value)
{
    steering_context_t *context = sensor->context;
    /* Return a hardware error on failure; write the measurement on success. */
    (void)context;
    *value = 0;
    return ESP_OK;
}

sensor_t steering_angle_sensor = {
    .name = "steering_angle",
    .can_id = CAN_ID_STEERING_ANGLE,
    .period_us = 20000, /* 50 Hz */
    .init = init_steering,
    .start = start_steering,
    .read = read_steering,
    .context = &steering,
};
```

Only `.name`, `.can_id`, `.period_us`, and `.read` are required by the current
sampler. Leave `.init`, `.start`, or `.context` unspecified when they are not
needed; static descriptor fields default to zero/`NULL`.

Callback meanings:

- `init` runs once during boot, before CAN can issue START. Configure GPIO,
  ADC, I2C, SPI, interrupts, and device registers here.
- `start` runs at the beginning of every new sampling session. Reset
  session-specific counters, timestamps, and filters here.
- `read` runs whenever the sampling period expires. It should take one bounded
  measurement or return a safely captured interrupt result.

Keep hardware handles and mutable state inside the driver. If an interrupt and
`read` share state, protect it with the appropriate ESP-IDF critical section or
other synchronization mechanism.

### 3. Add the source to CMake

Add the path under `SRCS` in `src/CMakeLists.txt`:

```cmake
"sensors/steering_angle.c"
```

Without this step, the descriptor will be declared in the registry but its
definition will not be linked into the firmware.

### 4. Add the descriptor to the existing registry branch

In `src/sensors/sensor_registry.c`, declare the exported descriptor in the
desired configuration and enlarge that branch's array:

```c
#elif NODE_FIXED_ENCODER_CONFIG
extern sensor_t bearing_encoder_sensor;
extern sensor_t steering_angle_sensor;

static sensor_t sensors[2];
```

Then populate both elements in `sensor_registry()`:

```c
#elif NODE_FIXED_ENCODER_CONFIG
sensors[0] = bearing_encoder_sensor;
sensors[1] = steering_angle_sensor;
```

The array size and number of assignments must agree. Registry order normally
does not matter. It does matter when several descriptors share one hardware
snapshot, as the MPU driver does: its first descriptor performs the physical
read and later descriptors return values from that snapshot.

No `platformio.ini` change is needed when reusing the same configuration and
physical node ID.

### 5. Update the Master

Update `../Master_Node/src/protocol/app_protocol.h` with the matching ID. Add it
to `protocol_is_sensor_id()` and map it to the correct `NODE_ID` in
`protocol_sensor_node_id()`.

Also update any Master signal-name, unit, CSV-column, dashboard, or decoding
table that enumerates supported sensor IDs. The protocol constant must match on
both sides exactly.

## Workflow B: create a new configuration from existing sensors

Use this when all drivers already exist, but a different firmware build should
contain a different combination of them.

### 1. Add a configuration branch

Choose a descriptive macro such as:

```c
NODE_FIXED_STEERING_CONFIG
```

Add a branch in both portions of `src/sensors/sensor_registry.c`:

```c
#elif NODE_FIXED_STEERING_CONFIG
extern sensor_t steering_angle_sensor;
extern sensor_t generic_adc_sensor;

static sensor_t sensors[2];
```

and:

```c
#elif NODE_FIXED_STEERING_CONFIG
sensors[0] = steering_angle_sensor;
sensors[1] = generic_adc_sensor;
```

Prefer an explicit branch for every configuration. Do not use a generic final
`#else` for a newly added configuration; an explicit macro makes mistakes much
easier to detect.

If the configuration uses shared support hardware, ensure it is initialized.
For example, the existing brake and generic-ADC configurations call
`adc_input_init()` from `sampler_init()`. A new ADC-using configuration must be
included in that compile-time condition, or the ADC reads will fail.

### 2. Add a PlatformIO environment

Add an environment to `platformio.ini`:

```ini
[env:NodeSteering]
extends = node_base
build_flags =
    ${node_base.build_flags}
    -D NODE_ID=7
    -D NODE_FIXED_BRAKE_CONFIG=0
    -D NODE_FIXED_ENCODER_CONFIG=0
    -D NODE_FIXED_ENGINE_CONFIG=0
    -D NODE_FIXED_ADC_CONFIG=0
    -D NODE_FIXED_MPU_CONFIG=0
    -D NODE_FIXED_STEERING_CONFIG=1
    -D SENSOR_SERIAL_TEST=0
```

Add the new macro with value `0` to every other environment as well. This makes
each build's selection visible and prevents settings from becoming ambiguous.

Rules:

- exactly one `NODE_FIXED_*_CONFIG` should be `1` in an environment;
- `NODE_ID` must be unique for a physical node on the CAN network;
- `SENSOR_SERIAL_TEST=0` is normal CAN-controlled operation; and
- `SENSOR_SERIAL_TEST=1` skips CAN and starts sampling immediately for bench
  testing. Do not use serial-test firmware on the operational CAN network.

### 3. Update the Master mapping

For every sensor transmitted by the new configuration, make sure the Master's
`protocol_sensor_node_id()` returns the new node ID. Add the node to any Master
registry or expected-node list used by the current firmware.

## Workflow C: add a completely new node and sensor

This is Workflow A followed by Workflow B:

1. Allocate the sensor CAN ID and document its integer unit/scale.
2. Write the sensor driver and descriptor.
3. Register its `.c` file in `src/CMakeLists.txt`.
4. Add a new branch to `sensor_registry.c`.
5. Add a PlatformIO environment with a unique `NODE_ID`.
6. Add the matching ID, accepted-sensor entry, and sensor-to-node mapping on the
   Master.
7. Update user-facing build, wiring, protocol, and signal documentation.
8. Build and bench-test the new profile before enabling normal CAN operation.

## Basic changes that do not require a new configuration

### Change a sample rate

Change the descriptor's `.period_us`:

```c
.period_us = 10000, /* 100 Hz */
```

The relationship is:

```text
period_us = 1,000,000 / samples_per_second
```

Examples:

| Rate | Period |
|---:|---:|
| 10 Hz | 100000 us |
| 25 Hz | 40000 us |
| 50 Hz | 20000 us |
| 100 Hz | 10000 us |
| 200 Hz | 5000 us |

Check comments and documentation whenever the numeric period changes. Also
consider CAN traffic and whether the physical transaction can finish within the
period.

### Change GPIO or bus settings

Change the constants in that sensor's driver. Recheck ESP32-C3 pin capability,
ADC channel mapping, voltage limits, pull-ups, and conflicts with CAN TX/RX or
other sensors in the same configuration. Update the wiring documentation.

### Change calibration, filtering, or units

Keep the conversion in the sensor driver. Do not modify the sampler. Preserve
the `int32_t` output contract and update both node and Master documentation or
decoding metadata so the same raw integer has the same meaning everywhere.

### Put one driver on two node configurations

Reuse the same exported descriptor in both registry branches. A descriptor is
copied into the selected registry at boot, so no wrapper driver is required.
Verify that its pins do not conflict with the other sensors in either build.

## Verification checklist

### Build checks

Build the changed environment:

```bash
pio run -e NodeSteering
```

Then build every environment whose registry or shared protocol code changed:

```bash
pio run -e NodeBrake
pio run -e NodeEncoder
pio run -e NodeEngine
pio run -e NodeADC
pio run -e NodeMPU
```

A protocol change should also be followed by a Master build.

### Serial bench check

Temporarily use a dedicated environment with:

```ini
-D SENSOR_SERIAL_TEST=1
```

Build, upload, and monitor it:

```bash
pio run -e NodeSteering -t upload
pio device monitor -b 115200
```

Verify the startup log lists every expected descriptor with its CAN ID and
period. Confirm values, sign, scale, stopped/disconnected behavior, and rate.

### CAN integration check

With `SENSOR_SERIAL_TEST=0`, verify:

1. the node boots and reports the intended node ID;
2. no sensor samples are sent before START;
3. every configured sensor begins reporting after START;
4. every configured sensor stops after STOP;
5. CAN IDs are accepted and attributed to the correct node by the Master;
6. timestamps, units, signs, and logging columns are correct; and
7. disconnects and hardware read errors fail in the documented way.

## Frequent mistakes

- The driver exists but is missing from `src/CMakeLists.txt`.
- The descriptor is declared in the registry but not copied into `sensors[]`.
- The registry array size does not match the number of assignments.
- More than one configuration macro is set to `1`.
- An ADC-based configuration does not initialize the shared ADC unit.
- The same CAN ID or node ID is assigned twice.
- The Sensor Node and Master use different protocol constants.
- The Master does not include the new ID in `protocol_is_sensor_id()`.
- The Master maps the sensor ID to the wrong physical node.
- The sampling-period number and its Hz comment disagree.
- A `read` callback blocks long enough to delay every later sensor in the
  registry loop.
- An interrupt modifies data that `read` accesses without synchronization.
- Serial test mode is accidentally left enabled for vehicle firmware.

## When a larger architectural change is needed

The descriptor pattern is appropriate for periodic scalar measurements that
fit in a signed 32-bit value. Revisit the protocol and sampler design before
adding a sensor that needs variable-length data, several frames per sample,
runtime discovery, per-sensor START/STOP control, or long blocking transactions.
Those requirements cannot be added safely by changing only a driver descriptor.
