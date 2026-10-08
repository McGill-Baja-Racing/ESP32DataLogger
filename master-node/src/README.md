# Master source map

Use this map to find where a change belongs. Read the [master introduction](../README.md) first; detailed runtime behavior is in [Architecture](../docs/MASTER_ARCHITECTURE.md).

## Firmware concurrency vocabulary

FreeRTOS schedules **tasks**, functions that run independently and usually wait or loop. A **queue** transfers messages between tasks so receiving data need not wait for slow storage. A **callback** is a function a module calls to hand an event to its owner. A **mutex** allows one caller at a time into a shared operation; a **critical section** protects short accesses to shared state.

An interrupt callback must do bounded work. Receiving a sample and finishing its SD write are different events. Preserve queue ownership and locking when changing behavior.

## Read in this order

1. `main.c`: startup order and routing between modules.
2. `app/app_control.c`: shared recording policy for serial and web calls.
3. The module for your task in the table below.
4. Its public header, then implementation and relevant host tests.

| Module | Owns | Change it for |
|---|---|---|
| `main.c` | Composition and application callbacks | Initialization and sample routing |
| `app/app_control.*` | Serialized lifecycle operations | Start/stop and live-view policy |
| `can/can_master.*` | TWAI, receive queue/dispatch, transmission, recovery | CAN transport |
| `node_state/node_registry.*` | Node state and traffic-based liveness | Monitoring and expected-node mask |
| `logger/data_logger.*` | Session files, log queue, writer, rename | [Recording/storage](logger/README.md) |
| `storage/sd_card.*` | Physical SD mount | SDMMC wiring/mount flags |
| `gps/gps_receiver.*`, `gps/rmc_parser.*` | UART reception and GPS decoding | Validated local telemetry |
| `time/absolute_clock.*`, `time/gps_time.*` | UTC anchor and calendar conversion | Absolute timestamps |
| `time/time_beacon.*` | Periodic clock/state broadcast | Node synchronization |
| `console/serial_console.*` | Text command parsing | Serial commands |
| `live/live_data.*` | Latest values and viewer lease | Live cache/rate policy |
| `web/` | Embedded page, HTTP controls/downloads | [Browser work](web/README.md) |
| `protocol/app_protocol.h` | Accepted IDs and node mappings | Shared [protocol](../../docs/PROTOCOL.md) |

## Data and control dependencies

```text
CAN RX → dispatch → main callback → registry + logger + live cache
GPS UART → parser → main callback → logger + live cache
serial/web commands → app_control → logger + CAN + beacon + registry
web downloads → completed files → CSV conversion or binary stream
```

Keep transport separate from session policy. Use public module APIs rather than accessing private state. `app_control` owns lifecycle ordering; its mutex coordinates competing serial/web actions. `main.c` connects modules rather than owning drivers.

Next: [Logger guide](logger/README.md), [Web guide](web/README.md), [Architecture](../docs/MASTER_ARCHITECTURE.md), or [Testing](../../docs/TESTING.md).
