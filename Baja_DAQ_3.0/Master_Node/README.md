# DAQ 3.0 Master Node

ESP32-P4 firmware that coordinates the Baja DAQ 3.0 sensor nodes and stores
their synchronized CAN samples on an SD card.

The bench CAN bus runs at 1 Mbit/s on master TX GPIO20/RX GPIO21 and Sensor
Node TX GPIO21/RX GPIO20.

## Responsibilities

- Mount the SD card and create numbered binary log files.
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

The master waits five seconds after boot before automatically opening a log and
broadcasting START, giving sensor nodes and serial monitors time to initialize.

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
to `http://192.168.4.1`. The page reports logger and node state, accepts Start
and Stop commands, and offers completed sessions as their original binary log
or as a streamed CSV conversion. Live Data supports up to four independent
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
`NodeMPU` is configured for CAN operation by default. Recording starts automatically
five seconds after initialization and can also be controlled through the web
interface or serial console.

### Engine and wheel RPM pairing

Node 4 sends bearing RPM every 20 ms (averaged over 100 ms). For each node 5
engine RPM frame, the master records a derived `engine_wheel_rpm` (0x0BD)
with the same timestamp, using the latest received node 4 measurement only
when it is at most 100 ms old in both sample time and receive time. A newer
wheel timestamp is not paired with an older engine sample. Missing/stale
wheel data produces no derived record; it is not replaced with zero.
The original bearing and engine records remain available. This is a recent
wheel-speed estimate, not wheel rotation measured between spark edges.
Node 5 reports zero engine RPM every 100 ms without sparks; wheel RPM remains
independent. Flash the updated master and NodeEngine; node 4 is unchanged.

### Quick CVT processing

Completed logs have a **CVT analysis** button after CSV. It runs the supplied
transmission analysis offline in your browser and exports a figure, processed
CSV and summary. See [settings, assumptions and tests](docs/CVT_ANALYSIS.md).

Use **CVT input CSV** to download only the two raw RPM channels in the exact
input schema used by `cvt_plot.py`. This differs from the processed results CSV.

**Paired RPM CSV** exports only `Timestamp,Engine RPM,Wheel RPM` (milliseconds),
with one complete recorded engine/wheel pair per row and no partial rows.

Live Data provides **Raw Bearing RPM**, **Secondary RPM** (raw × 3.389286), and
**Wheel RPM** (raw ÷ 3.589) as independently selectable views. CVT analysis uses
3.389286 as its editable default shaft ratio, including the bundled Python tool.
