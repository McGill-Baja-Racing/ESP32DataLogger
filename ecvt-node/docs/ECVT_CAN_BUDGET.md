# Preliminary centralized logging budget

Historical design/investigation record imported from ESP32_ECVT. For the
current standalone-first scope, see [Development](DEVELOPMENT.md); for executable
behavior, see [Terminal](../TERMINAL.md). Later bench results are in the
[November 25 report](bench/2026-11-25/README.md).

Design estimate, not measured performance or approved telemetry format.

## Updated debugging requirements

The user requests every motor command, every received servo feedback sample, and the exact sensor inputs used for each controller command, at least during initial development. Outside recording, keep only eCVT-required streams active. Periodic summary-only logging is not selected.

The five-stream illustration below therefore understates the intended debugging contract. Budget separately for multi-frame command snapshots and feedback. For example, if command snapshots require 8 frames at 50 commands/second and three feedback streams each produce 50 samples/second at one frame/sample, the added 550 frames/second consume about 7.43 percentage points. With 12 frames/snapshot at 50 commands/second and three feedback streams at 100 Hz, the added 900 frames/second consume about 12.15 points. These frame counts are provisional allowances, not an encoded format or measured feedback rate. Include statusword, mode/profile/zero changes, faults and manual commands separately where applicable.

Exact reconstruction needs a command identifier plus the values actually used (including filtered RPM), input provenance and freshness, controller state/target, and relevant configuration. Distinguish calculated target, attempted servo transmission, driver admission/completion, and feedback: CAN acknowledgement does not establish that the servo executed a command. Capture input snapshots consistently with computation rather than reading shared values again later in the logger.

Fragmented records need sequence/part identifiers and detection of incomplete records. Every received feedback sample is the requirement; no claim is made about samples the servo never transmitted or frames lost before reception. Full fidelity cannot be guaranteed through indefinite master disconnection or bus failure with finite queues. Confirmed policy: control continues on logging failure while required sensor inputs remain valid. Local fallback storage is being considered; no medium or capacity is selected. Any eventual backlog upload must be paced below live traffic and reconcile sequence IDs without duplicate records. CAN acknowledgement alone is not confirmation of master SD persistence; define an application acknowledgement if storage-confirmed fallback is required.

DAQ bus: 1 Mbit/s in the inspected shared protocol. Servo bus remains separate. Use a conservative planning allowance of 135 bits per standard 8-byte classical CAN frame including intermission and stuffing allowance, with no error/retry traffic. Added load = frames/second * 135 / 1,000,000. One such frame occupies about 135 microseconds. Queue, arbitration, task scheduling, and retry delays are additional.

| Added frames/second | Added bus utilization |
|---|---|
| 100 | 1.35 percentage points |
| 250 | 3.38 percentage points |
| 500 | 6.75 percentage points |
| 1000 | 13.50 percentage points |

Illustrative telemetry: five measurement/timestamp frames at 50 Hz = 250 frames/second, adding at most roughly 3.4 percentage points under these assumptions. At 100 Hz each, the same five streams add 6.75 points. Five streams are an example, not a complete replacement for current rich controller snapshots.

Inspected DAQ sensor rates imply 1250 periodic frames/second for two brake channels (200), six IMU channels (600), four shock channels (400), and bearing RPM (50), assuming all are active. This accounts for about 16.9% using the planning allowance. It excludes engine RPM and spark events, AiM, health/events, beacons, optional generic ADC, commands, errors, and retries. This is not the measured total bus load. Verify against the exact DAQ main baseline before implementing.

Reusing sensor broadcasts adds no per-consumer data frames. Only new eCVT telemetry adds normal data traffic. Servo command/feedback traffic stays on its separate bus unless deliberately summarized and published to DAQ.

Existing eCVT disk records are 44 bytes; raw fragmentation needs at least six 8-byte frames per record, before fragmentation metadata reduces usable payload. At 100 records/second this lower bound alone is 600 frames/second or 8.1% of bus capacity. Current logging also repeats sensor snapshots already available on DAQ. Prefer a deliberate telemetry contract over transporting every disk record unchanged, subject to the user's fidelity requirements.

Arbitration priority and burst size matter independently of average load. Propose telemetry IDs below sensor urgency (numerically higher IDs), periodic pacing, and a separate bounded transmit queue so master/logging failures cannot block servo control. These are proposals requiring agreement, including whether to drop stale telemetry or retain records. Record timestamps at acquisition/control time; master arrival time does not establish when remote values were sampled.

The current bearing input publishes at 50 Hz with a nominal 100 ms RPM averaging window. That measurement filtering latency already exists and may matter more than an individual CAN frame's serialization time. Evaluate control performance before changing that filter.

Validation: measure total bus use and bursts, source-to-controller input age, telemetry latency/drops, master receive/log queue losses, and servo-loop timing during SD writes/downloads and bus faults. Define acceptance thresholds with the user before declaring the design adequate.

Pending decisions: precise record encoding/rates; acquisition lifecycle and stream ownership outside recording; stale/missing input behavior; local fallback medium/capacity/overflow and acknowledgement policy; throttle calibration/node assignment; selected engine source and brake logic. Full-rate logging, exact command-input reconstruction, and continued control on logging-only failure are now requirements.
