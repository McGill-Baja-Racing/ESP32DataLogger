# DAQ 3.0 Master Node Architecture and Operation

## 1. Purpose and scope

The ESP32-P4 master is the coordinator and persistent data logger for the DAQ
3.0 CAN network. Its responsibilities are to:

- mount and manage the SD card;
- configure and control sensor nodes over 1 Mbit/s CAN/TWAI;
- receive, classify, and buffer CAN frames;
- record CAN and master-local measurements in binary log files;
- transmit a master time beacon every 100 ms;
- monitor node availability and recover from CAN bus-off conditions;
- expose serial commands for operation, diagnostics, and file download; and
- optionally sample GPS directly on the master.

The stable baseline deliberately contains only SD, CAN, and serial operation.
Wi-Fi, MQTT, cloud upload, and dashboards were removed until the logger
foundation passes the stable test gate.

Useful source references:

- [Master firmware entry point and task implementation](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L2883)
- [Master public control interface](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/master_control.h)
- [Sensor Node architecture and operation](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/docs/NODE_ARCHITECTURE.md)
- [Sensor node firmware](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c)
- [Stable build and test gate](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/docs/STABLE_BASELINE.md)

## 2. System architecture

The master uses a queue-based FreeRTOS pipeline so CAN reception, state
tracking, and relatively slow SD writes are isolated from one another.

```text
Sensor nodes
    |
    | CAN frames
    v
TWAI receive callback (interrupt context)
    |
    v
Main CAN RX queue
    |
    v
Dispatch task
    |-- node state and health tracking
    |-- heartbeat queue (reserved; no consumer yet)
    `-- SD sample queue
            |
            v
       SD writer task
            |
            v
       log_XXXX.bin
            |
            v
       decode_log.py -> CSV

GPS task -> master_submit_local_sample() -> SD sample queue
```

The TWAI callback performs only the minimum interrupt-safe work: it receives a
frame, packs its payload, and submits it to the main RX queue. The dispatch task
then performs node bookkeeping and forwards frames to the SD writer while
logging is active. See the [receive callback](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L1260)
and [dispatch task](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L2546).

## 3. Where configuration lives

DAQ 3.0 has three configuration layers. It is important to distinguish the
configuration stored in Git from the configuration that a flashed master is
actually using.

### 3.1 Build-time configuration

The master build profiles and feature flags are defined in
[`Master_Node/platformio.ini`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/platformio.ini).

The primary profiles are:

| Profile | GPS | SD node configuration | Intended use |
|---|---:|---:|---|
| `MasterStable` | Off | Off | Stable SD + CAN + serial baseline |
| `MasterStableGPS` | On | Off | Baseline plus GPS validation |

The flags control which source files are compiled and which tasks are started:

- `MASTER_CAN_ENABLED` enables the CAN interface and CAN-related tasks.
- `MASTER_AUTO_START_LOGGING` starts a recording 500 ms after boot.
- `MASTER_USE_SD_NODE_CONFIG` selects SD JSON instead of built-in node data.
- `MASTER_GPS_ENABLED` compiles and starts the GPS module.

`MASTER_RPM_ENABLED`, `MasterStableRPM`, and `MasterStableSensors` remain in
the repository only as experimental development paths. RPM did not operate
reliably in testing and is not a supported DAQ 3.0 feature. It must be
investigated and revalidated before it is enabled on a vehicle.

At present, every stable PlatformIO profile sets
`MASTER_USE_SD_NODE_CONFIG=0`. Therefore, the normal stable firmware does
**not** load the repository's `nodes_config.json`; it uses the built-in
configuration described below.

### 3.2 Built-in stable configuration

The configuration active in normal `MasterStable` builds is implemented by
[`load_default_node_config()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L685)
and its helper functions in `src/main.c`.

The built-in remote-node configuration is:

| Physical node | Sensor | CAN ID | Rate | Function |
|---|---|---:|---:|---|
| Node 1 | Front brake pressure | `0x0B1` | 100 Hz | `front_brake` |
| Node 1 | Rear brake pressure | `0x0B2` | 100 Hz | `rear_brake` |
| Node 4 | Bearing encoder | `0x0B9` | 50 Hz | `bearing` |

The same function also defines optional master-local signals:

| Sensor | CAN ID | Rate | Compiled task required |
|---|---:|---:|---|
| GPS speed | `0x700` | 1 Hz | GPS |
| GPS latitude | `0x701` | 1 Hz | GPS |
| GPS longitude | `0x702` | 1 Hz | GPS |
The configuration still reserves engine RPM ID `0x703` for future development,
but the current RPM implementation is not approved for use. GPS entries may
exist in memory while GPS is disabled, but no samples are produced unless the
GPS module is compiled and started.

### 3.3 SD JSON configuration

The human-editable system registry is
[`Master_Node/nodes_config.json`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/nodes_config.json).
For use on hardware, this file must be copied to:

```text
/sdcard/nodes_config.json
```

and the master must be built with:

```ini
MASTER_USE_SD_NODE_CONFIG=1
```

The JSON document is organized into four main areas:

```text
schema_version
description
protocol
    bitrate and shared command IDs
    runtime configuration payload layout
    node state acknowledgement layout
    sensor payload layout
master_sensors[]
    sensors physically attached to the master
nodes[]
    physical CAN nodes
        node identity and state CAN ID
        hardware description
        sensors[]
            sensor identity, CAN ID, units, rate, function and GPIO ports
```

Only entries with `enabled: true` are loaded. The master supports at most 16
physical nodes, eight sensors per node, and eight master-local sensors. It
validates and parses the file during boot. Configuration updates made through
the public API are written through a temporary file and backup, and can only be
applied while the logger is idle. See
[`master_save_node_config_text()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L1191).

### 3.4 Sensor-node configuration

Each physical sensor node has two kinds of configuration:

1. Its board identity and compiled capabilities come from
   [`Sensor_Node/platformio.ini`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/platformio.ini)
   and compile-time definitions such as `NODE_ID`.
2. Its enabled virtual sensors, CAN IDs, rates, functions, and ports are sent
   by the master at runtime before the node is started.

On every start or recovery, the master sends the node this sequence:

1. targeted STOP;
2. reset runtime sensor configuration;
3. one sensor definition per enabled sensor;
4. one sensor I/O definition per enabled sensor; and
5. targeted START.

The master-side sequence is implemented in
[`send_runtime_config_and_start_node()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L1496),
and the node receives it in
[`handle_node_config_frame()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c#L1198).

### 3.5 Configuration precedence

The effective configuration can be summarized as:

```text
PlatformIO build profile
    |
    `-- MASTER_USE_SD_NODE_CONFIG = 0
    |       `-- use built-in configuration in Master src/main.c
    |
    `-- MASTER_USE_SD_NODE_CONFIG = 1
            `-- load /sdcard/nodes_config.json
                `-- if missing or invalid, fall back to built-in configuration

Effective master configuration
    `-- transmitted to each physical node as runtime CAN configuration
```

The tracked JSON file is therefore the intended editable registry, while the
built-in table remains the currently active stable default and recovery path.

## 4. CAN protocol overview

Shared command IDs are defined near the top of both master and node firmware.

| CAN ID | Direction | Purpose |
|---:|---|---|
| `0x0A0` | Master -> nodes | Stop sampling/logging |
| `0x0A1` | Master -> nodes | Start sampling/logging |
| `0x0A2` | Master -> nodes | 64-bit master time beacon |
| `0x0A3` | Master -> nodes | Runtime sensor configuration |
| `0x0C0 + node_id` | Node -> master | Node active/low-power state |
| `0x180 + node_id` | Node -> master | Node health information |

Sensor frames contain an eight-byte little-endian payload:

| Bytes | Value |
|---|---|
| 0-3 | 32-bit sensor value |
| 4-7 | 32-bit timestamp in milliseconds |

CAN IDs and payload definitions are documented in the `protocol` section of
`nodes_config.json`, while the executable constants are in
[`Master_Node/src/main.c`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L90)
and
[`Sensor_Node/src/main.c`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Sensor_Node/src/main.c#L161).

## 5. Master FreeRTOS tasks

FreeRTOS schedules larger priority numbers before smaller ones.

| Task | Priority | Use |
|---|---:|---|
| `sd_writer` | 10 | Buffers 100 records, writes them to SD, and drains/closes the log during stop. |
| `save_task` | 10 | Every 15 seconds, closes and reopens the current file to improve durability. |
| `serial_cmd` | 9 | Handles operator commands and serial file download. |
| `can_recovery` | 9 | Polls TWAI every 250 ms and initiates bus-off recovery. |
| `dispatch` | 8 | Classifies received frames, tracks node status, and forwards log records. |
| `time_beacon` | 7 | Broadcasts the 64-bit master microsecond time every 100 ms. |
| `node_watchdog` | 6 | Detects three seconds of node silence and reconfigures the node during logging. |
| `gps` | 6 | Optional NMEA parser and GPS sample producer. |
| `auto_start_log` | 5 | Waits 500 ms, starts logging, and then deletes itself. |
| `rpm_sampler` | 4 | Experimental RPM task retained for future investigation; not supported for operation. |

Task creation is shown in
[`app_main()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L2994).

## 6. Master boot and start workflow

### 6.1 Boot workflow

`app_main()` performs this sequence:

1. Configure SDMMC and mount the card at `/sdcard`.
2. Load the selected built-in or SD node configuration.
3. Create the CAN RX, heartbeat, and SD sample queues.
4. Create the log-file mutex.
5. Initialize 1 Mbit/s TWAI on TX GPIO 20 and RX GPIO 21.
6. Send three global STOP commands so nodes enter a known idle state.
7. Create the baseline FreeRTOS tasks.
8. Start the optional GPS task if enabled. Experimental RPM builds are not part
   of the supported startup path.
9. Create the auto-start task when auto-start is enabled.

If SD mounting fails, the current firmware exits `app_main()` and does not
start CAN or the serial command task.

### 6.2 Logging start workflow

Automatic start occurs 500 ms after boot. An operator can also issue `start`
over serial. [`master_start_logging()`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/src/main.c#L1775)
then:

1. verifies that the logger is idle;
2. selects the next unused `/sdcard/log_XXXX.bin` name;
3. opens the new binary file;
4. clears stale entries from the SD sample queue;
5. changes the logger state to `RUNNING`;
6. resets node activity/recovery tracking; and
7. sends the effective runtime configuration and START command to every node.

The node watchdog subsequently retries any configured node that remains
offline or becomes silent.

### 6.3 Logging stop workflow

When `stop` is issued, the master:

1. changes state to `STOPPING`, preventing new samples from entering the log;
2. sends targeted STOP commands to configured nodes;
3. waits up to one second for low-power acknowledgements;
4. lets the SD writer drain its queue;
5. writes any partial block; and
6. closes the file and returns to `IDLE`.

## 7. Serial operating workflow

The serial monitor runs at 115200 baud. Commands are:

| Command | Action |
|---|---|
| `start` | Create a new log and configure/start nodes. |
| `stop` | Stop nodes, drain buffered data, and close the file. |
| `status` | Show logger state, node state, SD files, and capacity. |
| `files` | List SD files and capacity. |
| `download <filename>` | Transfer an idle file as Base64. |
| `log <mode>` | Select `off`, `master`, `status`, `node`, `samples`, or `all` diagnostics. |
| `help` or `?` | Print the command list. |

Recommended operating sequence:

1. Flash `MasterStable` and the appropriate `NodeStable` builds.
2. Insert a FAT-formatted SD card.
3. Boot and confirm the SD, configuration, and TWAI initialization messages.
4. Allow auto-start or issue `start`.
5. Issue `status` and verify that expected nodes become active.
6. Use `log status` for low-rate diagnostics; avoid verbose per-sample output
   during high-rate operation.
7. Issue `stop` before removing power or the SD card.
8. Use `files` and `download <filename>`, or read the card directly.
9. Decode the log with `tools/decode_log.py`.

## 8. Binary log format

Every record is 16 bytes:

```text
int64 little-endian: CAN ID
int64 little-endian: packed CAN payload
    low  32 bits: sensor value
    high 32 bits: timestamp_ms
```

The writer buffers 100 records (1,600 bytes) per block and flushes after each
block. During stop it also writes any incomplete block. The decoder is
[`tools/decode_log.py`](https://github.com/McGillBajaRacing/ESP32DataLogger/blob/main/Baja_DAQ_3.0/Master_Node/tools/decode_log.py).

Example:

```bash
python3 tools/decode_log.py log_0001.bin
```

## 9. Upgrade roadmap

### Priority 1: correctness and data-loss prevention

- Redesign and validate RPM acquisition before restoring it. The previous
  implementation did not work reliably, and its built-in GPIO 21 assignment
  also conflicts with the master's CAN RX pin.
- Check every `xTaskCreate` result in `app_main()` and fail visibly when a
  required task cannot start.
- Count and report failures when the interrupt callback cannot enqueue a CAN
  frame; this loss is currently silent.
- Either implement or remove the heartbeat queue. Its CAN ID is still a TODO,
  and there is no queue consumer.
- Filter logged frames so node state, health, and control traffic is not decoded
  as ordinary sensor measurements unless explicitly requested.
- Calculate and reject unsafe CAN bus loads before applying a configuration.
  Multiple 1 kHz signals in the example JSON can consume most or all of a
  1 Mbit/s bus.

### Priority 2: configuration and format consistency

- Enable SD configuration in a dedicated build profile and test configuration
  loading, rejection, backup, and rollback.
- Use one generated protocol definition for master, nodes, JSON documentation,
  and decoding tools instead of maintaining duplicated constants.
- Correct the JSON protocol description to document `aux_port` in runtime I/O
  byte 5 and keep supported function codes synchronized with node firmware.
- Generate decoder signal metadata from the configuration instead of the
  current hard-coded `SIGNAL_METADATA` table.
- Add a versioned log header containing firmware revision, active
  configuration, session start, and record format.
- Validate duplicate CAN IDs, conflicting GPIOs, unsupported functions, and
  unreasonable sample rates before starting nodes.

### Priority 3: resilience and observability

- Provide a degraded diagnostic mode when the SD card is missing, rather than
  preventing CAN and serial startup entirely.
- Replace fixed configuration delays with acknowledgements and an explicit
  node configuration state machine.
- Add task stack high-water marks, heap usage, queue peak depth, RX losses, and
  SD latency to health reporting.
- Add automated long-duration logging, CAN bus-off, queue-overflow, SD-removal,
  and repeated start/stop tests.
- Replace 32-bit millisecond record time with a session-defined 64-bit timebase
  for long recordings and clearer cross-node synchronization.

### Priority 4: features after the stable gate

- Restore Wi-Fi/MQTT telemetry and dashboards using the existing control and
  JSON status APIs.
- Add OTA firmware and configuration deployment.
- Add automatic log indexing, retrieval, and upload.
- Define a common producer interface for additional master-local sensors.

## 10. Stable validation order

Validate additions incrementally:

1. `MasterStable` -- SD + CAN + serial; and
2. `MasterStableGPS` -- add GPS.

RPM profiles are excluded from the stable validation order until RPM hardware,
signal conditioning, GPIO assignment, and firmware processing are redesigned
and validated.

Each stage should repeatedly mount the SD card, auto-start logging, create and
decode a valid log, survive serial stop/start cycles, and operate on CAN for at
least 30 minutes without reset.
