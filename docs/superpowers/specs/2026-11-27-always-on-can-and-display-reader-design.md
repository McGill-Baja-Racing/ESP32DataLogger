# Always-on CAN data and the driver display's CAN reader

Design, 2026-11-27.

**Branch:** `lucasl/driver-screen-integration`. It is the driver-screen PoC (`lucasl/driver-screen-poc`, which only touches `driver-display/`) replayed onto current `main`, and it holds both the firmware and the display work. The repository is squash-merge only: if the PoC is merged on its own first, rebase this branch onto the new `main`.

## Why

The driver display, a Raspberry Pi 3 B+ in the cockpit, has to show live values from the DAQ's CAN bus, whether or not the master is recording. Today the sensor nodes only sample and send after the master says "recording". Outside a recording, the bus carries only health, state and beacon frames, so the display would have nothing to show.

The display is also a **second way to debug the DAQ** when something goes wrong in the car, next to the master's web page. So it has to read and show everything on the bus, not only the driver's values.

## Goals

1. **Nodes always send.** Sensor nodes sample and send at their current rates from boot, with or without a master on the bus.
2. **The master handles constant traffic correctly.**
   - Start/Stop only controls writing to the SD card.
   - Node online/offline tracking and the web live view work all the time.
3. **Step 1, the display reads the bus.**
   - The display program reads the DAQ through SocketCAN and decodes every frame type with the shared `protocol/app_protocol.h`.
   - It keeps a live table of the bus.
   - A text tool, `can-monitor`, proves it works.
4. **Step 2, the screen shows live data.** The driver view and the workshop views are fed from that table instead of hardcoded values.
   - This document only outlines step 2.
   - Step 2 gets its own design pass once step 1 works on the real bus.

## Non-goals

- **No transmitting from the display.** It is a listen-only reader, needs no node ID, and is not tracked by the master.
- **No reduced idle rate.** Nodes send at full rate all the time (approach A below).
- **No new signals.** Speed, fuel and lap data are not on the bus today. Step 2 decides what to show for them.
- **No format changes.** The logged file formats, health frame layouts and CAN IDs stay as they are.

## Anti-goals: failures even if it "works"

- **The display disturbs the DAQ bus.** It must stay listen-only, especially while its crystal is wrong.
- **A recording loses data because of the always-on traffic.** This covers the master's RX queue overflowing at Start, and Start or Stop missing samples.
- **The master shows false health errors because of idle traffic.** For example, idle events counted as `HEALTH_EVENT_LOST`.
- **The screen shows an old or zero value as if it were live.** Missing data is shown as unavailable, as the display README already requires.
- **Frames the Pi missed look like a node fault.** The display reports its own losses separately.

## Constraints

- **Bitrate.** The bus runs at 1 Mbit/s. `CAN_BITRATE` is frozen in the protocol header, and the AiM EVO4S also talks at 1 Mbit/s.
- **The Pi's crystal is too slow.** The Pi's MCP2515 module has an 8 MHz crystal, which tops out at 800 kbit/s (MCP2515 datasheet DS20001801J, section 5.3 and table 13-2).
  - Before the display can join the bus, it needs a **16 MHz crystal** and `oscillator=16000000` in `/boot/firmware/config.txt`.
  - Until then, all display work is tested on the bench (see Testing).
  - The module runs on 3.3 V without the voltage dividers, and its bench checks pass (see `driver-display/hardware/can-module/README.md`).
- **No protocol header code changes.** `protocol/build_hooks.py` hashes the header without its comments, and updates over CAN between firmware versions rely on that hash. Comment and documentation changes are fine.
- **Mixed firmware keeps working.** A node still on old firmware stays silent until Start, exactly as today.
- **Bus load** is about 1,300 to 1,500 frames/s, roughly 20% of the bus.
  - That is the same as during a recording today, just continuous.
  - The master's RX queue (2048 frames) holds about 1.4 s of traffic. Its log queue (32768 entries) holds about 22 s.
- **The Pi may drop frames: a known issue, left for later (decided by Lucas).**
  - **The cause:**
    - the MCP2515 has two receive buffers;
    - the Linux `mcp251x` driver accepts every ID;
    - the overlay reads it over SPI at 1 MHz, so one frame read takes about 100 µs, and at 1 Mbit/s frames can arrive every 130 µs.
  - **Under full DAQ load, the Pi could miss many frames.**
  - **This work assumes the Pi reads the bus correctly.** It does nothing to reduce losses: no SPI speed change, no hardware change. Lucas tests it himself, and lowers the load while testing if needed.
  - **The display only shows the Pi's own loss counters** (see the live table), so losses can't be mistaken for node faults.
  - **Possible fixes later:**
    - raise `spimaxfrequency` (the MCP2515 accepts up to 10 MHz);
    - use a controller with a deeper receive buffer, such as a CAN HAT with an MCP2518FD.
  - The display README and the CAN module README record this limitation.

## Approaches considered

**Firmware idle rate**
- **A. Full rate always (chosen).**
  - It is the simplest change, and exactly the recording load the master already handles.
  - It gives the best data for debugging.
- **B. Reduced rate when idle, full rate while recording.**
  - The bus is quieter and the Pi drops fewer frames.
  - But every node needs a second sampling mode, the liveness thresholds must handle two rates, and the display is slower outside recordings.

**Display reader**
- **A. Drain a non-blocking SocketCAN socket every 10 ms from an LVGL timer (chosen).**
  - No threads and no locks, which fits LVGL being single-threaded.
  - The kernel's socket buffer holds frames while the screen draws.
- **B. A dedicated reader thread with a locked shared table.**
  - Slightly lower latency, which doesn't matter for a screen refreshing about 30 times per second.
  - It adds locking.

**Display role**
- **Listen-only (chosen).**
- **A real node that sends its own health frames (deferred).** It would need a new node ID (node 9 would collide with `0x0C9`), changes to the master, and a working transmitter.

## Decided behaviour

### Sensor nodes (`sensor-node/`)

**Sampling and recording state**
- **Sampling from boot.** The sampler starts right after initialisation in every vehicle build. Today only serial-test builds do that (`main.c:55`).
- **Start, Stop and the beacon's recording bit no longer start or stop the sampler.** They still:
  - set the node's CAN state (`current_state` in `can_node.c`);
  - send the state report on `0x0C0 + node` with its reason, exactly as today;
  - follow the beacon's recording bit within 100 ms.
- **The state byte and the status `mode` bit change meaning.** They now mean "the master is recording", not "this node is sampling".
  - The master's web page doesn't show either of them today. Its node list shows off, offline, waiting or active, from `node_registry_state_name()`, and that stays as it is.
  - The display shows each node's recording state from its state reports.

**Before the first beacon**
- Nodes send even with no master on the bus.
- Until the first beacon, samples carry the node's own clock (offset 0). The status already flags this with `HEALTH_NO_TIME_SYNC`.
- The master doesn't log before Start, so unsynchronised samples never reach a log.

**Health frames**
- **Events (`0x0E0 + node`) are still sent only while recording.** A node sends them only while the master is recording and time is synced. The check becomes "the master is recording" instead of `sampler_is_active()`.
- **Idle counts are dropped at Start (decided by Lucas).** With always-on sampling, nodes count problems all the time while idle, for example an unplugged sensor on the bench, or samples delayed while another node is updated.
  - At Start, each node resets every code's count of occurrences not yet reported.
  - The log then only describes problems from inside the recording.
  - The event sequence number is not reset: no events go out while idle, so the master sees no gap.
  - Problems before Start stay visible live: the 1 Hz status still shows ongoing faults, including during the recording.
- **Status and diagnostics stay at 1 Hz, always**, as today.

**Driver resets at Start**
- These currently run at every Start:
  - discarding the engine's queued captures;
  - resetting the bearing encoder's window;
  - invalidating the MPU's last reading.
- They now run once, when sampling starts at boot.

**Engine node (5).** It sends RPM zeros every 100 ms whenever the engine is stopped, recording or not.

**Updates over CAN**
- `begin_update` still refuses with `UPDATE_ERROR_RECORDING` while the master is recording.
- Otherwise the node **pauses its sampler** for the update, because flash writes would stall sampling and produce dropped-sample events.
- The sampler resumes if the update aborts or fails. A successful update reboots the node anyway.

**Bus-off.** The node still discards queued samples and counts them, as today.

### Master (`master-node/`)

**SD logging: unchanged**
- Samples are admitted only in `LOGGER_RUNNING`.
- Health admission keeps its 200 ms grace after Stop.
- The file formats don't change.

**The master's own health counts**
- These are faults such as `HEALTH_NODE_OFFLINE` for a node that is switched off in the workshop, or `CAN_RX_OVERFLOW` during an update.
- They are reset at Start in the same way as the nodes' counts, so `log_events()` only writes what happened during the recording.
- Its live health summary (`/api/health`) is unchanged and still shows them while idle.

**Node registry**
- Monitoring is on from boot and stays on.
- Start and Stop no longer toggle it or reset `data_seen` and `last_seen`.
- A node is offline after 3 s without any frame.
- A node sending only health frames shows "waiting", as an old-firmware node will while idle.

**Live data**
- `/api/live/samples` works while idle:
  - the 409 "available only while recording" error goes away;
  - so does the `live_data_force_stop()` call at Stop.
- The web page enables the live view whatever the logger state.
- AiM frames always reach live data, and reach the log only while running.

**Start and Stop must not block CAN dispatch**
- Today, CAN dispatch takes the lifecycle mutex for each frame. An unnamed Start can hold that mutex for up to 3 s while it waits for GPS to name the file.
- **Requirement:** frame dispatch never waits on Start or Stop for more than about 50 ms.
- The builder chooses how: for example, move the GPS wait outside the mutex, or keep dispatch off that mutex entirely.

**RX queue at Start.** Start no longer clears the CAN RX queue, which would throw away fresh live and health frames.
- **My call:** samples received just before Start may enter the log, with timestamps up to the queue latency before the Start time. That is harmless.

**Boot.** The 3 Stop bursts at boot and the 5 s auto-start stay as they are.

### Display, step 1 (`driver-display/`)

**Code layout**

| File | Role |
|---|---|
| `src/can/daq_decode.c/.h` | Pure functions: one CAN frame in, one typed record out. No I/O. Builds and runs on macOS and Linux |
| `src/can/daq_state.c/.h` | The live table: frames in with their arrival time, the current picture of the bus out. No I/O |
| `src/can/can_socket.c/.h` | Linux only. Opens a raw SocketCAN socket, drains it without blocking, reads the interface's counters and state |
| `src/can_monitor.c` | The terminal tool, built from the three above as `build/can-monitor` on Linux |

**Using the protocol header**
- Include it as `#include "protocol/app_protocol.h"`, with the repository root on the include path, as both firmwares do.
- **Decode bit fields with explicit shifts and masks**, following the layouts documented in the header, instead of casting to its packed bit-field structs.
- A host test checks that the shifts give the same result as the structs, so a header change that moves fields fails the test.

**What the decoder understands**
- **Master:** Stop, Start, and the beacon (recording flag, master uptime).
- **Node states:** state, reason, and reset reason.
- **Every sensor sample:** value, node timestamp, signal name and unit.
- **Health frames:**
  - status: faults, health level, mode, and the six sensor slots;
  - events: code, error, subject, sequence and count;
  - diagnostics: TX and RX error counters, resets and reset reason.
- **Firmware identity:** ELF hash prefix, protocol hash, pending flag.
- **Updates over CAN:** commands, data and replies.
- **AiM `0x500`:** RPM and counter.
- **Anything else** is "unknown", kept with its raw bytes.

**The live table**
- **Per signal:** latest value, arrival time on the Pi's monotonic clock, frame count, and measured rate.
  - A signal is stale, and shown as unavailable, when nothing arrived within a timeout.
  - **My call:** 500 ms for sensor signals and AiM, which is 5 periods of the slowest periodic stream (the engine's RPM zeros, every 100 ms).
- **Per node, 1 to 8:**
  - online or offline, by the master's rule of 3 s without any frame;
  - the last state report and its reason;
  - the last status and diagnostics;
  - the firmware identity;
  - the last 16 events, and a count of gaps in the event sequence.
    - **My call:** the gap count is shown next to the Pi's loss counters, not as a node health problem. The Pi can't tell a frame it missed from an event the node really lost.
- **Master:** whether a beacon was seen, the recording flag, master uptime, time since the last beacon, and the last Start or Stop seen.
- **The Pi's own losses, shown apart from node faults:**
  - socket drops (`SO_RXQ_OVFL`);
  - controller overruns and receive errors (`/sys/class/net/can0/statistics`);
  - the interface state (error-active, passive, bus-off), from error frames or netlink;
  - the bitrate and whether listen-only is on.

**Timestamps.** The nodes' timestamps are decoded and shown, but freshness, rates and trends use only the Pi's arrival time.

**Reading**
- **The display program** drains the non-blocking socket completely from an LVGL timer every 10 ms.
- **`can-monitor`** reads in its own loop and redraws the terminal once per second.
  - Its interface defaults to `can0` and can be changed.
  - `--raw` prints each decoded frame on its own line, for logging to a file.

**Bringing `can0` up (my call)**
- A systemd oneshot unit brings `can0` up at boot, listen-only, at 1 Mbit/s, with `restart-ms 100`.
- A Makefile target installs it, next to `autostart-on`, so the display program needs no root.
- The unit's bitrate is a parameter, so the bench can use 500 kbit/s until the 16 MHz crystal is fitted.

### Display, step 2 (outline only)

- **Driver view: engine RPM.** From `0x0BB`, shown as unavailable when stale. Step 2 decides whether to prefer AiM's `0x500`.
- **Driver view: brakes.** Front and rear pressures from `0x0B1` and `0x0B2`, shown as unavailable when stale.
- **Driver view: speed, fuel, CVT and lap stats.** They have no source yet and show as unavailable.
- **Workshop views.**
  - Live values and trends come from the table.
  - The CAN diagnostics view shows the per-node data and the Pi's own losses described above.

## Testing

**Firmware: host tests**
- Run with `python3 -m unittest discover -s master-node/tests`.
- **Update** the tests that encode the old gating:
  - `test_recording_lifecycle.py`: live data is no longer forced off at Stop, and the RX queue is no longer cleared at Start.
  - `test_health_recording.py`: health admission and its grace are unchanged.
- **Add** tests for:
  - registry monitoring on from boot;
- idle health counts, on nodes and on the master, dropped at Start;
  - live data accepted while idle;
  - no `HEALTH_EVENT_LOST` while idle;
  - dispatch not blocked during an unnamed Start.

**Firmware: builds.** Every master and node PlatformIO profile must compile.

**Firmware: on the bench or in the car**
- Nodes send from power-up with the master off.
- The master's web page shows live values and online nodes while idle.
- Recordings are complete from Start to Stop: no `HEALTH_EVENT_LOST`, and no RX overflow at Start.
- An update over CAN of an idle node works while the other nodes keep sending.
- A mix of old and new firmware works.

**Display: host tests**
- `make test` runs the decoder and table tests on macOS and Linux.
- They use golden frames:
  - the protocol example, `0x0B1` `DC 05 00 00 E8 03 00 00`, which is 1500 psi at 1000 ms;
  - at least one frame of every type in the protocol.

**Display: Pi bench, before the new crystal**
- A traffic generator replays realistic DAQ traffic onto `vcan0`, or onto `can0` in MCP2515 loopback mode:
  - the beacon at 10 Hz;
  - every node's samples at their real rates;
  - health frames at 1 Hz;
  - events and state reports.
- `can-monitor` must show every node, signal and rate correctly, and show "unavailable" when a stream stops.

**Display: real bus, after the 16 MHz crystal**
- Listen-only, in the car.
- `can-monitor` matches the master's web page.
- Nothing appears in the master's CAN error counters.
- Lucas tests the Pi's frame loss himself (see Constraints). It is not an exit criterion for this work.

## Rollout

Flash the master and every node from the same commit, over CAN from the web page or with USB. A node left on old firmware keeps working, but stays silent until Start.

## Documentation to update

**Firmware:** everything that describes the gating.
- `docs/PROTOCOL.md`, `docs/SYSTEM_OVERVIEW.md`, `docs/TESTING.md`, `docs/TROUBLESHOOTING.md`, `docs/WEB_TUTORIAL.md`
- `sensor-node/README.md`, `sensor-node/docs/NODE_ARCHITECTURE.md`, `sensor-node/docs/ADDING_SENSORS_AND_NODES.md`
- `master-node/README.md`, `master-node/docs/MASTER_ARCHITECTURE.md`, `master-node/docs/AIM_EVO4S.md`, `master-node/src/logger/README.md`
- The comment in `sensor-node/src/main.c`.

**Display**
- `driver-display/README.md`: the CAN status, and open problem 1, which this solves.
- `driver-display/docs/RASPBERRY_PI.md`: the CAN setup.
- The CAN module README, which also gets committed along with `tools/can_check.py`.

## Stop and ask Lucas before

- changing anything in `protocol/app_protocol.h` beyond comments;
- changing a log file format, CAN ID, rate or health layout;
- anything that makes the display transmit;
- changing what Start/Stop means for the SD log;
- a fix that needs a new dependency on the Pi or in the ESP32 builds.

## Left to the builder

- How dispatch is kept off the Start/Stop mutex.
- How the sampler pauses and resumes for updates over CAN.
- How the Pi reads the interface state: error frames or netlink.
- The layout of `can-monitor`'s output.
- The traffic generator's language: C, or Python with no dependencies.
- File names within the layout above.
