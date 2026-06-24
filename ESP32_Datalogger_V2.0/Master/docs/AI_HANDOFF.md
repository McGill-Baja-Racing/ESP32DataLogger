# Baja Logger AI Handoff

Generated: 2026-06-01

This document is meant to bring a new AI/chat up to speed quickly on the ESP32 Baja data logger project. It summarizes the architecture, key files, message protocols, current behavior, and known constraints.

## Project Shape

There are three cooperating parts:

1. **Master firmware**: ESP32-P4 logger, in this `Master/` folder.
2. **Node firmware**: ESP32-C3 sensor node, in sibling folder `../Node/`.
3. **Computer/cloud tools**: Streamlit dashboard in `tools/live_dashboard.py`, plus a small DigitalOcean/Docker cloud API in `cloud/`.

High-level flow:

```text
Sensor node(s) -> CAN/TWAI -> Master ESP32-P4 -> SD binary log
                                      |
                                      +-> Wi-Fi/MQTT/HTTP -> dashboard/cloud
```

The SD card is the source of truth for recorded logs. MQTT live telemetry is a preview/control channel and can drop/reconnect without changing the SD log.

## Build Commands

From `Master/`:

```bash
/Users/remylaurendeau/.platformio/penv/bin/pio run
```

From `../Node/`:

```bash
/Users/remylaurendeau/.platformio/penv/bin/pio run
```

Run dashboard from `Master/`:

```bash
.venv/bin/streamlit run tools/live_dashboard.py
```

## Master Firmware

Important files:

- `src/main.c`: SD card, CAN/TWAI, logger state, node runtime config, serial commands, JSON formatting helpers.
- `src/telemetry.c`: MQTT command/telemetry bridge, HTTP log upload, file list/config/download responses.
- `src/wifi_control.c`: ESP-Hosted Wi-Fi setup for the P4/C6 combination.
- `src/master_control.h`: API that `telemetry.c` uses to call into `main.c`.
- `nodes_config.json`: human-editable node/sensor registry that is loaded from SD on the master. It also contains master-local virtual sensors such as GPS.
- `platformio.ini`, `partitions.csv`, `sdkconfig*`: build and ESP-IDF config.

### Master Boot Behavior

On boot, the master:

1. Initializes SD card.
2. Loads `/sdcard/nodes_config.json`; if missing, uses built-in fallback.
3. Initializes TWAI/CAN.
4. Sends startup `STOP` commands to force nodes idle.
5. Starts Wi-Fi/MQTT telemetry.
6. Waits for serial/MQTT commands.

The master does **not** send full sensor configuration on boot. It sends runtime node/sensor config when logging starts.

### CAN Error Recovery

Both Master and Node firmware poll the TWAI controller state every 250 ms. If a
controller enters bus-off, its recovery task calls `twai_node_recover()` and
waits for the controller to return to error-active.

While logging, the Master also watches each configured node:

- Node health frames update the node's active/idle state.
- A node is marked offline after 3 seconds without traffic.
- An idle or offline node is sent its targeted runtime configuration and START
  command every 3 seconds until it returns.
- After the Master's own CAN controller recovers from bus-off, all nodes are
  revalidated and reconfigured because commands may have been lost.

The Node keeps its sampling state through a temporary CAN outage, clears stale
queued samples when bus-off occurs, and sends a state ACK after recovery.

### Start/Stop Behavior

`start` command:

1. Opens a new `/sdcard/log_XXXX.bin`.
2. Sends runtime config to each enabled node:
   - reset node virtual sensors
   - send one config frame per enabled sensor
   - send log mode config
   - send targeted start
3. Logger enters `running`.
4. CAN frames are saved to SD and optionally streamed to MQTT preview.

`stop` command:

1. Sends targeted stop to active nodes.
2. Drains queued samples.
3. Saves/closes/reopens log file.
4. Logger returns to `idle`.

### CAN Protocol

Shared master/node command IDs:

```text
0x0A0 STOP command
0x0A1 START command
0x0A2 master time beacon
0x0A3 node runtime config
0x0C0 + node_id node state ACK
0x180 + node_id node health
```

For the current test sensors:

```text
node 2: data 0x0B3 steering_angle, 0x0B4 brake_pressure, 0x0B5 wheel_rpm; state 0x0C2; health 0x182
node 3: data 0x0B6 steering_angle, 0x0B7 brake_pressure, 0x0B8 wheel_rpm; state 0x0C3; health 0x183
node 4: data 0x0B9 steering_angle, 0x0BA brake_pressure, 0x0BB wheel_rpm; state 0x0C4; health 0x184
node 5: data 0x0BC steering_angle, 0x0BD brake_pressure, 0x0BE wheel_rpm; state 0x0C5; health 0x185
```

Node state ACK reason values are `1=boot`, `2=stop`, `3=start`, and
`4=CAN recovery`.

Runtime config frame `0x0A3`:

```text
byte 0      target node_id, or 0xFF broadcast
byte 1      command: 0 reset, 1 sensor config, 2 log config, 3 sensor IO config

For sensor config:
byte 2      sensor_id
byte 3      flags, bit 0 = enabled
bytes 4-5   uint16 little-endian sample_rate_hz
bytes 6-7   uint16 little-endian CAN data ID

For sensor IO config:
byte 2      sensor_id
byte 3      function: 0 sim, 1 adc, 2 rpm, 3 front_brake, 4 rear_brake, 5 old_rpm
byte 4      port/GPIO number
bytes 5-7   reserved
```

Sensor data frame payload is 8 bytes:

```text
bytes 0-3   uint32 little-endian sensor value
bytes 4-7   uint32 little-endian timestamp_ms
```

Master binary SD log records are 16 bytes each:

```text
uint64 little-endian CAN ID
uint64 little-endian packed CAN data
```

The dashboard/cloud decoder assumes this 16-byte record format. The first field can now be either a real received CAN ID or a master-local virtual signal ID, such as GPS speed `0x700`, latitude `0x701`, or longitude `0x702`.

## Node Firmware

Important file:

- `../Node/src/main.c`

The current node firmware supports up to 8 runtime-configured virtual sensors per physical node. Each sensor can be enabled/disabled and assigned its own sample rate, CAN ID, function, and port by the master at start time. Supported functions are `sim`, `adc`, `rpm`, `old_rpm`, `front_brake`, and `rear_brake`. `rpm` uses a GPIO edge interrupt; `old_rpm` uses the legacy ADC threshold path.

ADC sensors use the ESP-IDF oneshot driver with per-channel eFuse calibration
and report calibrated millivolts. Set the sensor `function` to `adc`, its
`port` to the connected GPIO number, and its units to `mV`. The working
reference hardware/code used GPIO3 (`ADC_CHANNEL_3`). Sensor-specific
conversions such as millivolts to pressure should be applied separately from
the ADC conversion.

Brake pressure sensors use calibrated ADC millivolts and convert to integer PSI
on the node. Select `function` as `front_brake` or `rear_brake` and set `port`
to the ADC GPIO. The node currently defaults each brake calibration to
0.5-4.5 V sensor output, 0-2000 psi span, and the 2.33/4.33 resistor-divider
scale. Front and rear have separate constants in `../Node/src/main.c` so they
can be tuned independently.

Current simulated sensors for node 2:

```text
sensor_id 20 steering_angle  CAN 0x0B3 units deg_x10
sensor_id 21 brake_pressure  CAN 0x0B4 units psi_x10
sensor_id 22 wheel_rpm       CAN 0x0B5 units rpm
```

Example physical sensor config:

```json
{
  "sensor_id": 23,
  "name": "front_left_wheel_rpm",
  "enabled": true,
  "can_id": "0x0BC",
  "units": "rpm",
  "sample_rate_hz": 100,
  "function": "rpm",
  "port": 4
}
```

Example brake sensor config:

```json
{
  "sensor_id": 24,
  "name": "front_brake",
  "enabled": true,
  "can_id": "0x0BF",
  "units": "psi",
  "sample_rate_hz": 100,
  "function": "front_brake",
  "port": 3
}
```

The node no longer samples on coarse FreeRTOS ticks. It uses microsecond deadlines from `esp_timer_get_time()` so 1000 Hz requests become a 1000 us period. Node `sdkconfig*` files have `CONFIG_FREERTOS_HZ=1000`.

At high rates, the node pushes samples into `sample_tx_queue`; the higher-priority `send_task` transmits them over TWAI. If the queue fills, the node drops the oldest queued sample so newer data stays fresher.

Useful node logs:

```text
CONFIG sensor=20 ... rate=1000Hz period=1000us
START received target=2 ...
TX STATE | node=2 state=active reason=start
HEALTH | active=true load=... late_max=...us missed=... q=...
```

Nodes publish health every 500 ms. The Master routes node health frames through
a dedicated telemetry queue so high-rate sample traffic cannot overwrite them
before MQTT publication.

## Node Configuration

Primary config file:

```text
/sdcard/nodes_config.json
```

Checked-in template:

```text
Master/nodes_config.json
```

The current checked-in config enables nodes 2, 3, 4, and 5 and sets all three simulated sensors on each node to 1000 Hz for stress testing. It also defines master-local GPS signals under `master_sensors`:

- `gps_speed`, signal ID `0x700`, units `kph_x100`, `preview_enabled: true`
- `gps_latitude`, signal ID `0x701`, units `deg_e7`, `preview_enabled: false`
- `gps_longitude`, signal ID `0x702`, units `deg_e7`, `preview_enabled: false`

The master does not have a physical CAN node ID. Master-local sensors use CAN-style signal IDs only so they can share the existing SD log and dashboard graph paths.

Important distinction:

- `enabled`: configured/allowed by master config.
- `active`: current observed runtime state from node state ACK.

Changing `nodes_config.json` alone does not reconfigure physical nodes immediately. The new node config is applied when the master sends runtime config on the next `start`.

For master-local GPS, the config is applied when the master loads/reloads `nodes_config.json`. GPS position is recorded to SD when logging is running, but position is not shown in live graphs unless its `preview_enabled` flag is set true. Speed is previewed by default.

## MQTT and Cloud

Current broker/cloud server:

```text
MQTT broker: 138.197.132.56:1883
Cloud API:   http://138.197.132.56
Token:       baja_logger_test_v1
```

Master loads MQTT broker override from:

```text
/sdcard/mqtt_config.json
```

Example:

```json
{
  "broker": "mqtt://138.197.132.56:1883"
}
```

MQTT topics:

```text
baja/logger/master/status
baja/logger/master/can
baja/logger/master/health
baja/logger/master/files
baja/logger/master/download
baja/logger/master/config
baja/logger/master/command
```

Dashboard sends commands to:

```text
baja/logger/master/command
```

Example command payload:

```json
{"cmd":"start","token":"baja_logger_test_v1"}
```

Common commands:

```text
start
stop
files
download
config_get
config_save
config_reload
log
```

## Dashboard

Main file:

```text
tools/live_dashboard.py
```

Pages:

- **Live**: live CAN values and stacked charts.
- **Health**: master/node health table and optional health charts.
- **Files**: list SD files, request upload/download, server storage/delete.
- **Config**: edit/reload/save/apply `nodes_config.json`.
- **Nodes**: node/sensor UI for editing configuration.

The dashboard uses a background MQTT client. Incoming messages are placed into a queue and drained into Streamlit session state on refresh.

Auto-refresh behavior:

- Live/Health refresh normally.
- Files/Config/Nodes pause refresh unless a download is active.
- This was done to stop Streamlit from glitching/scroll-jumping and becoming slow.

## File Download / Upload Logic

Older MQTT base64 chunk download still exists in code, but current preferred path is HTTP raw upload:

1. Dashboard sends MQTT `download` command with file name and request_id.
2. Master opens the SD file.
3. Master streams it to cloud:

```text
POST http://138.197.132.56/upload/raw
```

Headers include filename, size, token, request ID.

4. Cloud stores the `.bin`, converts it to `.csv`, and returns URLs.
5. Master publishes `upload_progress` and final `upload_end` on `baja/logger/master/download`.
6. Dashboard fetches the CSV URL and offers direct CSV download.

Current HTTP upload tuning in `src/telemetry.c`:

```text
stream buffer: 32 KB
HTTP TX buffer: 16 KB
progress step: 256 KB
yield step:    512 KB
```

The cloud raw-upload endpoint streams the incoming `.bin` to disk and converts
to CSV in 64 KB decode blocks so large logs do not need full-file in-memory
buffers during conversion.

Server-side uploaded files are retained on the Droplet and can be listed/deleted from the dashboard. Cloud retention limits are implemented in `cloud/api/main.py`.

## Cloud Server

Folder:

```text
cloud/
```

Important files:

- `cloud/docker-compose.yml`
- `cloud/mosquitto/mosquitto.conf`
- `cloud/api/main.py`
- `cloud/api/Dockerfile`

Services:

```text
cloud-mqtt-1  eclipse-mosquitto:2  port 1883
cloud-api-1   FastAPI/uvicorn      ports 80 and 8000
```

Deploy rough flow:

```bash
rsync -av cloud/ root@138.197.132.56:/opt/baja-logger/cloud/
ssh root@138.197.132.56
cd /opt/baja-logger/cloud
docker compose up -d --build
docker compose ps
```

Check running API version:

```bash
docker exec cloud-api-1 python -c "import main; print(main.API_VERSION); print(main.__file__)"
```

## Health Metrics

Node health payload over CAN `0x180 + node_id`:

```text
byte 0 node_id
byte 1 flags: bit0 active, bit1 sample drops seen
byte 2 load_percent
byte 3 max_lateness_ms
byte 4 sample_queue_depth
byte 5 missed_deadlines
byte 6 tx_fail_count
byte 7 free_heap_kb
```

Note: node internally measures lateness in microseconds, but packs max lateness rounded to milliseconds for CAN health.

Master health is generated as JSON in `master_format_health_json()` and published by `telemetry.c`.

## Debug Logging Modes

The dashboard can send log mode changes. Modes:

```text
off
master
status
node
samples
all
```

Default should be quiet/off for performance. Turn logging on only for diagnosis because serial logging at high sample rates can heavily distort timing.

## Known Constraints / Gotchas

- At 3 sensors x 1000 Hz, the node attempts 3000 CAN frames/sec. This is an intentional stress test and may exceed practical CAN/logger throughput depending on bus/load.
- `nodes_config.json` changes are not sent to nodes until `start`.
- Master sends startup `STOP` on boot, not full runtime sensor config.
- If dashboard progress does not move, check that master firmware includes `upload_progress` publishing in `upload_raw_file()`.
- If cloud API looks old after deploy, verify the container code with `docker exec cloud-api-1 python -c "import main; print(main.API_VERSION)"`.
- If MQTT to a local Mac broker fails, the board and broker must be reachable over IP. The current robust setup uses the Droplet so the Mac and board do not need to be on the same LAN.
- ESP-Hosted logs may warn about version mismatch; system can still work, but RPC timeouts can occur.
- PlatformIO may warn that expected flash is 4 MB but found 2 MB. Builds have still passed; verify actual hardware/partition assumptions before adding much more firmware size.
- Master partition was enlarged previously because firmware exceeded 1 MB.

## Good First Questions For A New AI

When starting a new debugging session, first ask/verify:

1. Which side is being changed: Master, Node, dashboard, or cloud?
2. Was the master flashed after firmware edits?
3. Was the node flashed after node edits?
4. Is `/sdcard/nodes_config.json` updated, not only the checked-in template?
5. What does the master serial log say at `start`, especially `Config node=... rate=...Hz`?
6. What does the node serial log say, especially `CONFIG sensor=... period=...us`?
7. Is MQTT connected and are `/status`, `/health`, and `/download` messages arriving?
