# CAN protocol and signal representation

This is the shared wire-contract reference. Read the [system overview](SYSTEM_OVERVIEW.md) first; use the [documentation index](README.md) to return to learning guides.

## Transport and commands

CAN uses standard 11-bit identifiers at 1 Mbit/s. TWAI is Espressif's CAN interface. Pin assignments are in [Hardware](HARDWARE.md).

| ID | Direction | Payload |
|---|---|---|
| `0x0A0` | Master → nodes | STOP |
| `0x0A1` | Master → nodes | START |
| `0x0A2` | Master → nodes | Eight-byte time and recording beacon |
| `0x0C0 + node ID` | Node → master | Three-byte state report |

The master sends zero-length global START/STOP frames in repeated bursts. Node receivers also accept a nonempty command whose first byte is their node ID or `0xFF` for all nodes. Repeated commands acknowledge the current state without repeating the sampler transition.

Every 100 ms, the master sends a little-endian unsigned 64-bit beacon. Bit 63 means recording is active; bits 0–62 contain elapsed master microseconds. Mask off the recording bit before computing the clock offset:

```text
clock_offset_us = master_time_us - node_local_time_us
sample_timestamp_ms = (local_sample_time_us + clock_offset_us) / 1000
```

A node follows beacon recording state as well as explicit commands. This allows restart after a reboot during recording. Elapsed time starts again when the master reboots; it is not UTC.

## State reports

| Byte | Meaning |
|---|---|
| 0 | State: idle `0`, active `1` |
| 1 | Reason: boot `1`, stop `2`, start `3`, CAN recovery `4`, beacon synchronization `5` |
| 2 | ESP reset reason for a boot report; otherwise zero |

State frames are control information, not logged sensor samples. During recording, sensor traffic supplies liveness; there are no separate heartbeat frames. The expected-node mask currently enables nodes 1, 3, 4, and 5. A configured node is offline after three seconds without sensor traffic. Node 6 is accepted but not enabled in that mask by default.

## Sensor samples

Every accepted CAN sensor frame has exactly eight bytes:

| Bytes | Representation |
|---|---|
| 0–3 | Little-endian signed 32-bit measurement |
| 4–7 | Little-endian 32-bit synchronized millisecond timestamp, interpreted as unsigned by the master |

| Source | IDs | Integer meaning | Acquisition |
|---|---|---|---|
| Node 1 | `0x0B1`, `0x0B2` | Front/rear pressure, psi | 100 Hz each |
| Node 3 | `0x0B3`–`0x0B5` | Acceleration X/Y/Z, milli-g | 100 Hz each |
| Node 3 | `0x0B6`–`0x0B8` | Angular velocity X/Y/Z, milli-degrees/second | 100 Hz each |
| Node 4 | `0x0B9` | Signed bearing RPM | 50 Hz, nominal 100 ms averaging window |
| Node 6 | `0x0BA` | Voltage, mV | 100 Hz |
| Node 5 | `0x0BB` | Engine RPM | Valid consecutive spark intervals; zero on stopped timeout |
| Node 5 | `0x0BC` | Spark event, value `1` | Detected rising edges |
| Master GPS | `0x700` | Speed, km/h × 100 | Valid GPS telemetry updates |
| Master GPS | `0x701`, `0x702` | Latitude/longitude, degrees × 10⁷ | Valid GPS telemetry updates |

GPS IDs identify local log/live samples; the GPS receiver does not send them over CAN. Engine capture sends spark events even when an interval is unsuitable for RPM. Stopped-engine zeros are generated at approximately 100 ms intervals. Queue pressure can lose events. The master rejects engine/spark frames whose timestamp has the sign bit set; long-session behavior is therefore more restricted for these channels than the unsigned timestamp format alone suggests.

## Changing this contract

Keep the [master header](../master-node/src/protocol/app_protocol.h) and [sensor header](../sensor-node/src/protocol/app_protocol.h) compatible. Also update master accepted-ID/node mappings, live/export metadata, offline decoding, and this table. IDs are occupied through `0x0BC`; choose a new ID only after checking both headers.

Build all firmware profiles when shared contracts change and perform a physical CAN test. [Adding sensors](../sensor-node/docs/ADDING_SENSORS_AND_NODES.md) gives the procedure; [Testing](TESTING.md) gives acceptance checks. The [log format](LOG_FORMAT.md) specifies how these samples are stored.
