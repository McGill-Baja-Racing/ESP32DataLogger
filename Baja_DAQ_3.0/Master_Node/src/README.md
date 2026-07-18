# Master Node source guide

The master uses the same module-ownership approach as the Sensor Node. Each
module owns its state and exposes a small API; `main.c` connects the modules and
contains only application-level policy.

## Directory map

```text
src/
├── main.c                    Startup and cross-module application flow
├── protocol/app_protocol.h   CAN commands, state base ID, and sensor IDs
├── can/can_master.*          TWAI transport, RX dispatch, TX, and recovery
├── console/serial_console.*  Parsing of start/stop/status commands
├── logger/data_logger.*      Log lifecycle, queue, blocks, and file writes
├── node_state/node_registry.* Latest state reported by each sensor node
├── storage/sd_card.*         ESP32-P4 SDMMC mount and pin configuration
└── time/time_beacon.*        Periodic master microsecond beacon
```

## Ownership and dependencies

- `can_master` owns the TWAI handle, receive queue, error counters, and CAN
  recovery tasks. It forwards complete frames to `main.c` through one callback.
- `data_logger` owns the file, mutex, log queue, writer task, and logger state.
- `node_registry` owns the latest reported state for every supported node.
- `sd_card` owns only physical SD initialization and mounting.
- `time_beacon` owns the periodic beacon task and uses the CAN public API.
- `serial_console` parses text but delegates actions through callbacks.
- `main.c` decides that starting means opening a log before starting nodes, and
  stopping means closing the session and stopping nodes.

```text
main -> sd_card
     -> data_logger
     -> can_master -> main RX callback -> logger or node_registry
     -> serial_console -> main lifecycle callbacks
     -> time_beacon -> can_master_send
```

## Compatibility with Sensor Node

The following values in `protocol/app_protocol.h` must match the Sensor Node:

| Purpose | CAN ID |
|---|---:|
| STOP | `0x0A0` |
| START | `0x0A1` |
| Master time | `0x0A2` |
| Node state | `0x0C0 + node ID` |
| Front brake | `0x0B1` |
| Rear brake | `0x0B2` |
| Bearing encoder | `0x0B9` |
| Generic ADC voltage | `0x0BA` |
| Engine RPM placeholder | `0x0BB` |

After changing the protocol, build `MasterStable`, `NodeBrake`, and
`NodeEncoder`, and `NodeEngine`. A successful build checks interfaces and types; a bench CAN test
is still required to validate timing and physical communication.

## Where changes belong

| Change | Location |
|---|---|
| Accepted sensors or protocol IDs | `protocol/app_protocol.h` |
| CAN pins, queues, or recovery | `can/can_master.c` |
| SD pins and mount options | `storage/sd_card.c` |
| Binary record buffering | `logger/data_logger.c` |
| Session start/stop ordering | `main.c` |
| Serial command parsing | `console/serial_console.c` |
| Time-beacon rate/encoding | `time/time_beacon.c` |
