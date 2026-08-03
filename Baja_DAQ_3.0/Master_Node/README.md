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
```

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

The accepted first-version signals are front/rear brake pressure, signed
bearing RPM, generic ADC voltage from node 6 (`0x0BA`), and the engine RPM
placeholder from node 5 (`0x0BB`). The placeholder is logged but currently
contains zero.
