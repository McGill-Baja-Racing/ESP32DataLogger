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

The offset is a receive/dispatch-time estimate, not a measured clock-accuracy
guarantee. `time_sync` starts with zero offset, so samples before the first beacon
use local elapsed time. It replaces the offset on every beacon without filtering,
latency compensation, or a synchronization-validity flag; timestamp continuity
and a precise cross-node error bound are not guaranteed. Periodic samples are
timestamped immediately before their read callback. Engine edges retain their
captured local time and are translated using the offset current when consumed.
Measure accuracy for applications that depend on tight timing.


## State reports

| Byte | Meaning |
|---|---|
| 0 | State: idle `0`, active `1` |
| 1 | Reason: boot `1`, stop `2`, start `3`, CAN recovery `4`, beacon synchronization `5` |
| 2 | ESP reset reason for a boot report; otherwise zero |

State frames are control information, not logged sensor samples. During recording, sensor and health traffic supply liveness. The expected-node mask enables nodes 1, 3, 4, 5, 7, and 8. A configured node is offline after three seconds without either kind of traffic; a responding node without recent sensor data shows `waiting`. Node 6 is accepted but not enabled in that mask by default.

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
| Node 4 | `0x0B9` | Signed gearbox bearing RPM | 50 Hz, nominal 100 ms averaging window |
| Node 6 | `0x0BA` | Voltage, mV | 100 Hz |
| Node 5 | `0x0BB` | Engine RPM | Valid consecutive spark intervals; zero on stopped timeout |
| Node 5 | `0x0BC` | Spark event, value `1` | Detected rising edges |
| Node 7 | `0x0BD`, `0x0BE` | Front linear pot 1 and 2, mm | 100 Hz each |
| Node 8 | `0x0BF`, `0x0C9` | Rear linear pot 1 and 2, mm | 100 Hz each |
| Master GPS | `0x700` | Speed, km/h × 100 | Valid GPS telemetry updates |
| Master GPS | `0x701`, `0x702` | Latitude/longitude, degrees × 10⁷ | Valid GPS telemetry updates |

Bearing RPM is the raw gearbox bearing measurement, not wheel or secondary RPM; drivetrain conversion happens later.

GPS IDs identify local log/live samples; the GPS receiver does not send them over CAN. Engine capture sends spark events even when an interval is unsuitable for RPM. Stopped-engine zeros are generated at approximately 100 ms intervals. Queue pressure can lose events. The master rejects engine/spark frames whose timestamp has the sign bit set; long-session behavior is therefore more restricted for these channels than the unsigned timestamp format alone suggests.

## Worked front brake example

This example illustrates the representation, not a new calibration measurement.
Suppose `adc_input_read_mv()` returns 1250 mV after ADC conversion. The current
front-brake driver computes sensor pressure with its divider factor `0.5`,
500–4500 mV sensor span, and 3000 psi range:

```text
pressure = (1250 - 500 × (0.5)) / (4000 × (0.5)) × 3000
         = 1500 psi → int32 value 1500 (positive fractions would be truncated)
```

If its synchronized timestamp is 1000 ms, standard CAN ID `0x0B1` carries eight
bytes `DC 05 00 00 E8 03 00 00`: little-endian 1500 followed by little-endian
1000. The node ID is not in these bytes; the master maps this signal ID to node
1. Follow the same sample into the [binary record and CSV](LOG_FORMAT.md).

## AiM EVO4S external input

Standard ID `0x500` at 1 Mbit/s carries four bytes: unsigned little-endian 16-bit RPM then unsigned 16-bit wrapping counter. The master accepts this independent source without DAQ node commands or clock synchronization. It creates local record `0x703` (`engine_rpm_aim`) using reception time and the normal value/timestamp layout. It is not a sensor-node ID and is never retransmitted. See [configuration and lifecycle](../master-node/docs/AIM_EVO4S.md).

## Node health

Flash the master and sensor nodes from this integration together. Every node sends
four-byte status and diagnostics payloads at 1 Hz on `0x0D0 + node` and
`0x0F0 + node`. The master adds its arrival timestamp to make eight-byte log
records. While recording, nodes also send eight-byte events on `0x0E0 + node`,
with the time of the last occurrence. Master health is logged locally as node 0.
The web health panel uses `/api/health`; AiM connection status stays separate.

Status packs faults, health level, mode, and six sensor states. Events pack code,
error, an eight-bit subject, sequence, count, and timestamp; the exact bit layout
is in the shared header. Sensor slots follow each node's CAN IDs in ascending
order, excluding engine spark. Both shock nodes have two slots. Subject aliases
`0x70`–`0x72` represent GPS IDs `0x700`–`0x702`; `0x73` and `0x74` represent
AiM wire ID `0x500` and normalized RPM ID `0x703`. This preserves those IDs in
drop/overflow diagnostics without changing the event layout.

For `node_offline` events, the subject is an offline-node mask: bits 1–7 retain
nodes 1–7, and the previously unused bit 0 represents node 8. The master is never
an offline subject. The firmware CSV exporter and offline decoder use the same
mapping. After STOP, normal samples stop entering the log immediately; health
admission closes after a 200 ms grace period, before the final queue drain.

## Changing this contract

Both firmwares include the same [shared header](../protocol/app_protocol.h) at the repository root. It owns CAN bitrate (`CAN_BITRATE`), beacon period (`CAN_MASTER_TIME_PERIOD_MS`), command/signal IDs, state-report length, node mask/capacity, and sensor-to-node mappings. Both CMake components include the repository root; retain `#include "protocol/app_protocol.h"` in source files. Update `protocol_sensor_node_id()` for a new signal; `protocol_is_sensor_id()` derives acceptance from that mapping. Also update live/export metadata, offline decoding, and this table. Sensor IDs occupy `0x0B1`–`0x0BF` and `0x0C9`; state reports occupy `0x0C1`–`0x0C8`. The rear second pot uses `0x0C9` to avoid the brake state report at `0x0C1`; choose a new ID only after checking the shared header and control/state ID ranges. Each concurrently deployed measurement source needs its own signal ID: distinct node IDs do not distinguish duplicated sensor IDs.

`CAN_NODE_ID_MASK` lists supported node IDs (currently 1, 3, 4, 5, 6, 7, and 8); `CAN_NODE_SLOT_COUNT` is 9, allowing indices 0–8. Sensor builds reject a `NODE_ID` absent from the mask. The mask must fit the slot count, and the master expected-node mask must be a subset of supported nodes. Supporting a node and enabling its monitoring are separate steps; see the extension guide for status/UI changes.

Build all firmware profiles when shared contracts change and perform a physical CAN test. [Adding sensors](../sensor-node/docs/ADDING_SENSORS_AND_NODES.md) gives the procedure; [Testing](TESTING.md) gives acceptance checks. The [log format](LOG_FORMAT.md) specifies how these samples are stored.
