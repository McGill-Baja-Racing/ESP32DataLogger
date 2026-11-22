# EVO4S backup engine RPM

The master listens for AiM on the common sensor CAN bus at 1 Mbit/s. AiM is an independent source, not a DAQ-controlled node. DAQ START/STOP commands do not control AiM acquisition or internal recording. Verify whether your configuration transmits while idle; keep AiM internal recording enabled as a separate backup.

## Race Studio 3 and wiring

Save the current AiM configuration before editing. Use CAN Output on CAN1 (ECU connector), a compatible CAN ECU Stream selection, and the physical RPM input channel rather than ECU RPM. Verify ignition pulses per revolution and RPM scaling.

| Setting | Value |
|---|---|
| Bitrate | 1 Mbit/s |
| Identifier | Standard 11-bit `0x500` |
| DLC | 4 |
| Byte order | Little endian |
| Bytes 0–1 | Unsigned 16-bit RPM, multiplier 1, offset 0 |
| Bytes 2–3 | Counter 0–65535, increment 1, wrapping |
| Output frequency | 20 Hz initially; 100 Hz only after device verification |

3000 RPM with counter 1 is `B8 0B 01 00`. The EVO4S manual documents 20 Hz; the later general CAN Output FAQ documents up to 100 Hz. Validate what the connected device accepts and actually transmits. Set RPM acquisition frequency at least as high as output where supported, and verify its response.

Connect ECU port pin 1 CAN1+ to CAN-H, pin 2 CAN1− to CAN-L, and pin 7 battery ground to the appropriate common ground reference. These are device external-view pin numbers; confirm the mating harness drawing. Use a short twisted-pair branch. Retain exactly the two bus endpoint terminators; disable AiM's internal 120 ohm resistor when both are already fitted. Do not use the GPS/Exp bus for this connection. Confirm no other installed device uses `0x500`.

References: [EVO4S manual](https://www.aim-sportline.com/download/doc/eng/evo4s/EVO4S_user_guide_eng.pdf), [CAN Output FAQ](https://www.aim-sportline.com/download/faqs/eng/software/rs3/FAQ_RS3_CAN-Output_100_eng.pdf), [pinout](https://www.aimsports.com/download/technical-sheets/aim_evo4s_tech_sheet.pdf).

## Master behavior

Valid frames update independent AiM status at all times. Each received frame carries its reception time through the receive queue. CAN dispatch takes the recording lock before dequeue and holds it through processing and log admission. START holds the same lock while preparing the session files, clearing the log and CAN queues together, and then enabling recording. This also discards pending node-status messages. A handler cannot hold an old frame across START/STOP; while idle it still decodes frames for health updates but does not save AiM measurements. Dispatch waits for traffic outside the lock and rechecks the queue after acquiring it. Already admitted logger records drain normally on STOP. Missing AiM never blocks recording or changes node 5 status.

Each valid frame during its active session becomes master-generated record `0x703`, `engine_rpm_aim`, source `aim_evo4s`, units `rpm`. It uses the existing eight-byte normalized payload: integer RPM then master milliseconds. This is different from the incoming four-byte `0x500` wire payload. Repeated RPM and zero remain valid. The wire counter is diagnostic only and is not stored as an RPM timestamp or a separate channel. Original engine RPM and Powertrain CSV remain unchanged; use full CSV to compare both sources. Binary records and UTC sidecars retain their existing formats.

The master timestamps CAN reception before queueing using its uptime clock. This is arrival time, not AiM sample time; it includes unknown measurement/filtering and output scheduling delays. There is no assumed time correction or synchronization of the AiM clock. Measure relative delay before claiming accuracy.

`/api/status` includes `aim` with state (`waiting`, `receiving`, `stale`), `last_frame_age_ms` (-1 before reception), `valid_frames`, `malformed_frames`, and `counter_discontinuities`. Status and counters last until master reboot. Freshness expires at 500 ms without a valid frame; malformed frames do not refresh it. Counter jumps/repeats/restarts are discontinuities, not a proven loss count; wrap is normal. The live API marks stale AiM samples and the browser labels the last value stale.

Master build flag `-D AIM_OUTPUT_RATE_HZ=100` changes nominal live metadata after 100 Hz verification; default is 20. Reception accepts either rate independently of that flag. No sensor-node firmware change is required.

## Before vehicle use

- Build all master profiles and run `python3 -m unittest discover -s tests`.
- Confirm 1 Mbit/s communication, byte encoding, counter wrap, and actual frame frequency with a capture or known input.
- Check AiM transmission while idle, recording, and restarting; compare RPM scaling to a reference.
- Start/stop/restart DAQ recordings with continuous AiM traffic. Verify both RPM channels in full CSV and offline decoding, and no samples from previous sessions.
- Disconnect/reconnect AiM: verify stale status after 500 ms, recovery, and no fabricated zero. Stop the engine and independently verify AiM's real zero behavior.
- Confirm existing nodes still obey START/STOP with AiM absent and present; inspect CAN/logger drops under load.
- Compare repeated controlled RPM transitions through both acquisition paths and measure lag/variation. Our 3.3 V simulator must not be assumed compatible with AiM's RPM input; use suitable signal conditioning.

These hardware checks cannot be established by host tests or compilation.
