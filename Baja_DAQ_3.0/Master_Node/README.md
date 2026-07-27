# DAQ 3.0 Master Node

ESP32-P4 firmware that coordinates the Baja DAQ 3.0 sensor nodes and stores
their synchronized CAN samples on an SD card.

The bench CAN bus runs at 1 Mbit/s on master TX GPIO20/RX GPIO21 and Sensor
Node TX GPIO21/RX GPIO20.

## Responsibilities

- Mount the SD card and create numbered binary log files.
- Broadcast START and STOP commands to fixed-configuration sensor nodes.
- Broadcast a 64-bit master-time beacon every 100 ms.
- Receive the configured sensor CAN IDs and buffer them to the SD card.
- Register node boot/start/stop/recovery state reports.
- Recover the CAN controller after bus-off.
- Provide `start`, `stop`, `testdata`, and `status` serial commands at
  115200 baud.

## Build

```bash
pio run -e MasterStable
```

The master waits five seconds after boot before automatically opening a log and
broadcasting START, giving sensor nodes and serial monitors time to initialize.

## CAN reliability test

While a log is running, enter `testdata` in the serial console. The command
injects five Sensor Node-format messages into the same receive queue used by
real CAN frames. Stop the log and decode it to verify the receive-to-SD path.

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
```

[src/main.c](src/main.c) is the composition root. Module ownership and change
guidance are documented in [src/README.md](src/README.md). The CAN payload and
log formats are described in [docs/MASTER_ARCHITECTURE.md](docs/MASTER_ARCHITECTURE.md).

The master and sensor nodes intentionally duplicate a very small protocol
header because they are separate firmware projects. Changes to CAN command IDs,
node-state encoding, or sensor IDs must be made in both protocol headers and
validated by building all firmware profiles.

The accepted first-version signals are front/rear brake pressure, bearing
encoder angle, generic ADC voltage from node 6 (`0x0BA`), and the engine RPM
placeholder from node 5 (`0x0BB`). The placeholder is logged but currently
contains zero.
