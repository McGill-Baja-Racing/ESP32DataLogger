# DAQ 3.0 Sensor Node Architecture and Operation

## 1. Purpose and scope

The DAQ 3.0 Sensor Node firmware runs on ESP32-C3 boards distributed around
the vehicle. A node converts one or more physical or simulated inputs into
timestamped CAN measurements. It is designed as a configurable sensor host:
the firmware build establishes the board identity and supported hardware
functions, while the master tells the node which sensors to enable, their CAN
IDs, sample rates, functions, and GPIO assignments.

The node is responsible for:

- receiving time, configuration, START, STOP, and log-mode commands;
- maintaining up to eight runtime-configured virtual sensors;
- sampling simulated, ADC, brake-pressure, acceleration, RPM, and quadrature
  encoder inputs when those capabilities are compiled in;
- timestamping samples on the master's timebase;
- buffering and transmitting samples over 1 Mbit/s CAN/TWAI;
- reporting active/idle state and health information; and
- recovering its CAN controller after a bus-off event.

Useful repository references:

- [Sensor Node firmware](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c)
- [Sensor Node build profiles](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/platformio.ini)
- [Master system configuration registry](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/nodes_config.json)
- [Master architecture and operation](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/docs/MASTER_ARCHITECTURE.md)

## 2. High-level architecture

The node separates input sampling from CAN transmission with FreeRTOS queues.
This allows sampling logic to continue on schedule while the CAN controller is
temporarily busy.

```text
Master CAN commands
    |
    v
TWAI receive callback (interrupt context)
    |
    v
CAN RX queue
    |
    v
Receive task
    |-- update master-time offset
    |-- reset/apply runtime sensor configuration
    |-- start or stop sampling
    `-- change diagnostic log mode

Physical/simulated inputs
    |
    | ADC reads, GPIO pulse ISR, encoder ISR, or generated waveform
    v
Sample task
    |
    v
64-entry sample TX queue
    |
    v
Send task
    |
    v
1 Mbit/s CAN bus -> Master -> SD log
```

The node does not write files or store a complete vehicle configuration. The
master remains the authoritative coordinator and logger.

## 3. Configuration model

Sensor-node operation is controlled by two configuration layers. Both must
agree for a sensor to work.

### 3.1 Build-time board identity and capabilities

[`Sensor_Node/platformio.ini`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/platformio.ini)
defines the physical identity, CAN pins, boot behavior, and sensor functions
compiled into the firmware.

Important definitions are:

| Definition | Purpose |
|---|---|
| `NODE_ID` | Physical address used for targeted commands and node-specific state/health CAN IDs. |
| `NODE_CAN_TX_GPIO` | TWAI transmit pin. |
| `NODE_CAN_RX_GPIO` | TWAI receive pin. |
| `NODE_START_ACTIVE_ON_BOOT` | Starts sampling without waiting for the master when set. |
| `NODE_DEFAULT_SENSORS_ENABLED` | Enables the built-in sensor table at boot. |
| `NODE_ENABLE_SIM_FUNCTION` | Compiles simulated sensor generation. |
| `NODE_ENABLE_ADC_FUNCTIONS` | Compiles ADC, pressure, acceleration, and legacy ADC-RPM functions. |
| `NODE_ENABLE_RPM_FUNCTION` | Compiles GPIO-interrupt RPM measurement. |
| `NODE_ENABLE_BEARING_FUNCTION` | Compiles quadrature bearing-encoder measurement. |

Available profiles are:

| Profile | Main purpose |
|---|---|
| `NodeStable` | Inactive-on-boot baseline; simulation capability only and no sensors enabled by default. |
| `NodeStableSim` | Enables the built-in simulated sensors for bench testing. |
| `NodeStableRPM` | Compiles the GPIO-interrupt RPM function. |
| `NodeStableADC` | Compiles ADC, brake-pressure, acceleration, and legacy ADC-RPM functions. |
| `NodeEncoderTest` | Standalone encoder test using GPIO 6/7 and alternate CAN pins. |

All current general profiles default to `NODE_ID=4`. Before deploying multiple
physical nodes, a profile must be created or overridden for each board's actual
node ID, pins, and required capabilities. A master request cannot enable a
function that was excluded from the node build.

### 3.2 Master-owned runtime sensor configuration

The vehicle-level node and sensor registry is maintained by the master in
[`Master_Node/nodes_config.json`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/nodes_config.json)
or in the master's built-in fallback table.

Before starting a node, the master sends:

1. a targeted STOP command;
2. a runtime sensor reset;
3. one sensor definition for every enabled sensor;
4. one sensor I/O definition for every enabled sensor; and
5. a targeted START command.

The sensor definition supplies:

- sensor ID;
- enabled state;
- sample rate; and
- CAN data ID.

The I/O definition supplies:

- sensor function;
- primary GPIO/port; and
- optional auxiliary GPIO/port.

The node accepts commands addressed to its `NODE_ID` or to broadcast address
`0xFF`. A zero-length START or STOP is also considered broadcast. Runtime
configuration is handled by
[`handle_node_config_frame()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c#L1198).

### 3.3 Effective configuration

```text
Sensor Node PlatformIO profile
    |-- physical NODE_ID and CAN pins
    `-- compiled sensor capabilities
             +
Master node/sensor registry
    |-- enabled sensor IDs
    |-- CAN data IDs and sample rates
    `-- functions and GPIO assignments
             |
             v
Effective runtime virtual-sensor table on the node
```

The build determines what the board *can* do. The master configuration
determines what it *will* do during the current logging session.

## 4. Supported sensor functions

The runtime protocol identifies sensor behavior with a function code:

| Code | Function | Input and output behavior |
|---:|---|---|
| 0 | `sim` | Generates deterministic test waveforms. |
| 1 | `adc` | Reports calibrated ADC input voltage. |
| 2 | `rpm` | Uses falling-edge GPIO interrupts and pulse spacing to calculate RPM. |
| 3 | `front_brake` | Converts calibrated ADC voltage to front brake pressure. |
| 4 | `rear_brake` | Converts calibrated ADC voltage to rear brake pressure. |
| 5 | `old_rpm` | Detects tach pulses through ADC threshold crossings. |
| 6 | `bearing` | Uses a quadrature encoder and reports angular displacement in degrees x10. |
| 7 | `accel_x` | Converts an analog accelerometer X axis to acceleration. |
| 8 | `accel_y` | Converts an analog accelerometer Y axis to acceleration. |
| 9 | `accel_z` | Converts an analog accelerometer Z axis to acceleration. |

ESP32-C3 ADC1 inputs are limited to GPIO 0 through GPIO 4. RPM uses one GPIO,
while the bearing encoder uses primary and auxiliary GPIOs. The master
configuration must avoid CAN-pin conflicts and assigning the same physical
input to incompatible functions.

## 5. CAN protocol and state control

The node and master share these standard 11-bit CAN identifiers:

| CAN ID | Direction | Purpose |
|---:|---|---|
| `0x0A0` | Master -> node | STOP command |
| `0x0A1` | Master -> node | START command |
| `0x0A2` | Master -> node | 64-bit master microsecond time beacon |
| `0x0A3` | Master -> node | Sensor, I/O, and diagnostic configuration |
| `0x0C0 + NODE_ID` | Node -> master | Node state acknowledgement |
| `0x180 + NODE_ID` | Node -> master | Node health report |

The executable protocol constants are defined in
[`Sensor_Node/src/main.c`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c#L161).

### State lifecycle

In normal builds, a node boots inactive and reports a low-power/idle state. It
still keeps CAN, receive, health, and recovery services running while idle.

```text
Boot
  |
  v
Idle / waiting for configuration
  |
  | targeted START
  v
Active / sampling and transmitting
  |
  | targeted STOP
  v
Idle
```

The state acknowledgement includes the current state, the reason for the
transition, and the ESP reset reason following boot. While already active, the
current firmware ignores a zero-length or `0xFF` broadcast STOP; the master's
normal session-stop sequence uses targeted STOP commands for each node.

## 6. Time synchronization

The master broadcasts its 64-bit microsecond clock every 100 ms. On receipt,
the node calculates:

```text
time_offset = master_time_us - local_time_us
```

When a sample is acquired, the node adds this offset to its local ESP timer and
converts the result to milliseconds. All sensor frames therefore use the
master's timebase rather than unrelated local boot clocks.

The current implementation applies the newest offset directly. It does not
yet filter beacon latency, estimate drift between beacons, or detect a stale
time source.

## 7. Sampling and transmission

The central sample task maintains an independent next-due time for each enabled
virtual sensor. At each deadline it:

1. obtains a master-synchronized timestamp;
2. reads or calculates the sensor value;
3. advances that sensor's next deadline; and
4. places the sample in the transmit queue.

Input-specific behavior includes:

- ADC functions use one-shot reads and ESP-IDF calibration when available.
- RPM edges are captured by a GPIO ISR; the scheduled sampler publishes the
  latest calculated RPM and returns zero after the no-pulse timeout.
- The bearing encoder updates a quadrature count on both GPIO edges; the
  sampler converts the count into angular displacement.
- Simulated signals use repeatable waveforms for network and logging tests.

The sample transmit queue holds 64 records. If it fills, the node removes the
oldest record and attempts to enqueue the newest, while incrementing its drop
counter. This favors recent measurements during temporary CAN congestion.

The send task packs each sample into an eight-byte payload:

| Bytes | Contents |
|---|---|
| 0-3 | Signed 32-bit sensor value, little-endian |
| 4-7 | Signed/unsigned 32-bit synchronized timestamp in milliseconds, little-endian |

Each configured sensor has its own CAN identifier. The master records the
identifier and complete payload without changing the node's value encoding.

## 8. Health reporting and fault recovery

Every 500 ms, the health task sends an eight-byte report on
`0x180 + NODE_ID`. It contains:

| Byte | Meaning |
|---:|---|
| 0 | Node ID |
| 1 | Active flag and sample-drop flag |
| 2 | Estimated sampling/transmit load percentage |
| 3 | Maximum sample lateness in milliseconds |
| 4 | Current sample queue depth |
| 5 | Missed-deadline count |
| 6 | CAN transmit-failure count |
| 7 | Free heap in kilobytes |

The master uses these frames to determine whether a node is alive and whether
it reports active operation.

The CAN recovery task polls the controller every 250 ms. When the controller
enters bus-off, the task clears queued samples and starts TWAI recovery. After
the controller returns to error-active state, the node reports its current
state with a recovery reason. The master's watchdog may then resend the full
runtime configuration and START sequence.

## 9. FreeRTOS tasks

Larger FreeRTOS priority numbers run before smaller ones.

| Task | Priority | Responsibility |
|---|---:|---|
| `can_recovery` | 8 | Detect and recover CAN warning, passive, and bus-off states. |
| `receive` | 8 | Process time, configuration, START, STOP, and log-mode frames. |
| `send` | 7 | Serialize queued samples and transmit them over CAN. |
| `health` | 3 | Report load, timing, queue, drop, transmit, and heap metrics every 500 ms. |
| `sample` | 2 | Schedule and acquire all enabled runtime virtual sensors. |
| `sample_adc` | 2 | Optional legacy dedicated ADC sampler; disabled by default. |
| `sample_rpm` | 2 | Optional legacy dedicated ADC-RPM sampler; disabled by default. |

CAN and GPIO callbacks run in interrupt context rather than as tasks. Task
creation is located in
[`app_main()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c#L2166).

## 10. Boot and operating workflow

### Normal boot

`app_main()` performs the following sequence:

1. Store and report the hardware reset reason.
2. Initialize compiled ADC, RPM, and encoder capabilities.
3. Create the 64-entry sample transmit queue.
4. Create the 16-entry CAN receive queue.
5. Create and enable the 1 Mbit/s TWAI controller.
6. Register the interrupt-driven receive callback.
7. Report low-power/idle state and wait for the master.
8. Start send, receive, recovery, health, and enabled sampler tasks.

### Session start

The master first resets and fills the node's virtual-sensor table. On targeted
START, the node clears stale queued samples, arms every sensor schedule, enters
the active state, and sends an active acknowledgement. Sampling then continues
until a targeted STOP is received.

### Session stop

On targeted STOP, the node leaves the active state, clears unsent samples, and
reports low-power/idle state. Sampling tasks remain allocated but wait until
the next START command.

## 11. Build and validation

From `Baja_DAQ_3.0/Sensor_Node`:

```bash
pio run -e NodeStable
pio run -e NodeStableSim
pio run -e NodeStableRPM
pio run -e NodeStableADC
pio run -e NodeEncoderTest
```

Recommended validation order:

1. Confirm the intended `NODE_ID`, CAN pins, and compiled capabilities.
2. Boot on a terminated CAN bus and confirm the idle state frame.
3. Verify time-beacon reception and synchronized timestamps.
4. Send runtime configuration and confirm the resulting sensor table.
5. Send targeted START and verify state, health, and sensor frames.
6. Send targeted STOP and verify that sample transmission ends.
7. Force CAN bus-off and verify autonomous recovery and master
   reconfiguration.
8. Run the final sample-rate configuration long enough to confirm no queue
   drops, deadline misses, or unexpected resets.

## 12. Upgrade priorities

### Priority 1: deployment correctness

- Add explicit production build profiles for each physical node instead of
  relying on all general profiles defaulting to Node ID 4.
- Validate at build or startup that CAN, ADC, RPM, and encoder GPIO assignments
  do not conflict.
- Check every `xTaskCreatePinnedToCore` result and fail visibly if a required
  task cannot start.
- Count and report CAN RX queue failures from the interrupt callback; incoming
  frame loss is currently silent.
- Confirm whether ignoring broadcast STOP while active is the desired safety
  policy, and document or revise it consistently on both master and nodes.

### Priority 2: protocol and configuration consistency

- Generate shared protocol constants and function codes from one source for
  master firmware, node firmware, JSON, and decoding tools.
- Add explicit runtime configuration acknowledgement and rejection frames so
  the master knows whether every function and GPIO assignment was accepted.
- Validate CAN ID uniqueness, supported rates, and bus load before START.
- Version runtime configuration payloads to allow backward-compatible firmware
  changes.

### Priority 3: timing and data quality

- Filter time-beacon offset measurements, detect missing beacons, and report
  synchronization validity and estimated error.
- Replace the 32-bit millisecond timestamp with a session-defined 64-bit
  timebase for long recordings.
- Store sensor calibration coefficients in configuration rather than fixed
  firmware constants.
- Define and document scaling for every sensor output, especially acceleration
  and pressure values.

### Priority 4: resilience and maintainability

- Protect runtime sensor-table updates from concurrent sampler access with a
  mutex or configuration handoff mechanism.
- Add unit tests for payload parsing, sensor scheduling, simulated waveforms,
  pressure conversion, RPM filtering, and quadrature decoding.
- Add hardware-in-the-loop tests for queue overload, noisy inputs, master
  resets, missing time beacons, and repeated START/STOP cycles.
- Remove or consolidate the legacy dedicated ADC/RPM tasks once the generic
  virtual-sensor path is fully validated.
