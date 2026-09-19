# DAQ 3.0 Master Node

ESP32-P4 firmware that coordinates the Baja DAQ 3.0 sensor nodes and stores
their synchronized CAN samples on an SD card.

The bench CAN bus runs at 1 Mbit/s on master TX GPIO20/RX GPIO21 and Sensor
Node TX GPIO21/RX GPIO20.

## Responsibilities

- Mount the SD card and create safely named binary log files without overwriting
  an existing session.
- Broadcast START and STOP commands to fixed-configuration sensor nodes.
- Broadcast a master-time beacon with recording state every 100 ms so rebooted
  nodes can rejoin an active session.
- Receive the configured sensor CAN IDs and buffer them to the SD card.
- During recording, use received sensor frames to mark nodes active and report
  a node offline after three seconds without data.
- Select installed nodes with `MASTER_EXPECTED_NODE_MASK` in
  `src/node_state/node_registry.h` (currently nodes 4 and 5).
- Recover the CAN controller after bus-off.
- Provide `start`, `stop`, and `status` serial commands at 115200 baud.
- Host an open `BajaDAQ` Wi-Fi access point with controls and log downloads at
  `http://192.168.4.1`.
- Provide explicitly enabled live sensor graphs during recording at a capped
  1-20 Hz display rate.

## Build

```bash
pio run -e MasterStable
pio run -e MasterNoCAN  # GPS/SD/web master without CAN hardware
```

`MasterNoCAN` does not initialize TWAI, transmit node commands or time beacons,
or monitor CAN nodes. GPS logging, SD sessions, serial commands, Wi-Fi controls,
downloads, and live GPS graphs remain available.

The master automatically starts recording five seconds after boot. An unnamed
start waits up to three additional seconds for GPS UTC and uses
`log_YYYY-MM-DD_HH-MM-SS.bin` when available; otherwise it falls back to the
first free `log_XXXX.bin`. A repeated timestamp receives a numeric suffix
instead of overwriting an existing log. The web filename field stays empty
while idle; entering a name before Start uses that custom filename instead.

`SD_FORMAT_IF_MOUNT_FAILED` in `platformio.ini` controls automatic formatting.
It defaults to `1`, so an inserted card that cannot be mounted as FAT is erased
and formatted. Set it to `0` for non-destructive mount failures.

## Data flow

```text
Sensor CAN frames -> can/can_master.c -> main dispatch
                                            |-- node state -> node_registry
                                            `-- sensor data -> data_logger
                                                                  |
                                                                  v
                                                               SD card

time/time_beacon.c -> can_master_send() -> synchronized sensor nodes
console commands   -> main policy       -> logger + node commands
web controls       -> main policy       -> logger + node commands
```

## Wi-Fi controls

The Waveshare ESP32-P4-WIFI6 uses its ESP32-C6 coprocessor over SDIO through
ESP-Hosted. Connect a phone or laptop to the open `BajaDAQ` network and browse
to `http://192.168.4.1`. The page reports logger and node state, lets the driver
name a session before Start, accepts Stop commands, and offers completed sessions as their original binary log
or as a streamed CSV conversion. A three-dot menu beside each completed log
can rename its binary file and UTC companion without overwriting another log.
Completed logs are listed in descending alphabetical order and can be filtered
by date, custom, or numbered names. Live Data supports up to four independent
graphs with adjustable axes. It must be enabled separately during each
recording and turns off when recording stops or its viewer disconnects. The SD
log continues to retain samples at their native rates.

The C6 must run an ESP-Hosted slave firmware compatible with the version pinned
in `dependencies.lock`. Network startup failures are reported on serial and do
not disable SD logging, CAN collection, or serial commands.

[src/main.c](src/main.c) is the composition root. Module ownership and change
guidance are documented in [src/README.md](src/README.md). The CAN payload and
log formats are described in [docs/MASTER_ARCHITECTURE.md](docs/MASTER_ARCHITECTURE.md).

The master and sensor nodes intentionally duplicate a very small protocol
header because they are separate firmware projects. Changes to CAN command IDs,
node-state encoding, or sensor IDs must be made in both protocol headers and
validated by building all firmware profiles.

The master accepts the following sensor channels for SD logging, live graphs,
and CSV export:

| Sensor | Source | IDs | Units |
| --- | --- | --- | --- |
| Front/rear brake pressure | CAN node 1 | `0x0B1–0x0B2` | psi |
| Acceleration X/Y/Z | CAN node 3 (MPU6500) | `0x0B3–0x0B5` | mg |
| Gyroscope X/Y/Z | CAN node 3 (MPU6500) | `0x0B6–0x0B8` | mdps |
| Wheel/bearing speed | CAN node 4 | `0x0B9` | signed rpm |
| Generic ADC | CAN node 6 (optional) | `0x0BA` | mV |
| Engine speed | CAN node 5 | `0x0BB` | rpm |
| GPS speed, latitude, longitude | Master UART | `0x700–0x702` (log IDs) | km/h × 100, degrees × 10⁷ |

CAN sensor messages must contain exactly 8 bytes: a little-endian signed
32-bit value followed by a little-endian unsigned 32-bit timestamp in ms.
Nodes 1, 3, 4, and 5 are monitored for missing data during recording.
GPS uses UART1 at 9600 baud, RX GPIO33 and TX GPIO32, and accepts
checksum-valid NMEA RMC sentences with an active position fix.

Build `Master` or `MasterStable` for the full sensor setup (`MasterNoCAN`
only collects local GPS). CAN uses TX GPIO20, RX GPIO21, at 1 Mbit/s.
Build the corresponding sensor profiles with `SENSOR_SERIAL_TEST=0`;
`NodeMPU` is configured for CAN operation by default. Recording starts
automatically five seconds after initialization and can also be controlled
through the web interface or serial console.

### Engine and wheel RPM across nodes

NodeEngine measures engine RPM from spark input GPIO3 and sends `engine_rpm`
(0x0BB) and spark events (0x0BC). NodeEncoder independently measures bearing
RPM on GPIO6/GPIO7 and sends `bearing_rpm` (0x0B9) every 20 ms, averaged over
100 ms. The sensors run on separate boards. The master logs and displays
these original channels without generating another wheel RPM signal.
NodeEngineBench uses the shared throttled serial format for engine RPM only.

### Powertrain CSV export

**Powertrain CSV** downloads `log_XXXX_rpm_paired.csv` with the columns
`Timestamp,Engine RPM,Wheel RPM,Car Speed (km/h)`. Timestamp is the engine sample time in
milliseconds. Each engine sample uses the latest previously recorded bearing
RPM sample, provided it is no more than 100 ms old. Missing, future, or stale
wheel samples omit the row; zeros and signed RPM values are preserved.
Wheel RPM is the raw bearing reading, without gear scaling.

Car Speed (km/h) holds the latest GPS speed recorded before the engine record.
It is blank if no GPS reading has a timestamp at or before that sample; there is
no age cutoff.

Endpoint: `/api/logs/download?name=log_0001.bin&format=paired`.
Validate the exporter with `python3 tests/test_paired_csv.py`.

Full CSV exports retain `timestamp_ms` and add `absolute_time_utc` from GPS.
See [GPS absolute time](docs/ABSOLUTE_TIME.md) for wiring, accuracy, and UTC companion files.
