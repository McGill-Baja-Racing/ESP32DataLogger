# Display CAN Reader (Step 1): Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the Pi reads the DAQ bus listen-only through SocketCAN, and decodes every frame type with the shared protocol header into a live table of the bus. A terminal tool, `can-monitor`, shows that table.

**Architecture:**
- **Three layers in `driver-display/src/can/`:**
  - a pure decoder: one frame in, one typed record out;
  - a pure live table: frames plus arrival time in, current picture out;
  - a Linux-only socket layer.
- **Host tests** cover the two pure layers on macOS and Linux.
- **A Python traffic generator** drives `vcan0` on the Pi so `can-monitor` can be checked before the 16 MHz crystal is fitted.

Feeding the screen from this table is step 2 and is not in this plan.

**Tech Stack:**
- C11, built with the existing `driver-display/Makefile` (`gcc` on the Pi, `cc` on macOS).
- Linux SocketCAN.
- Python 3 standard library only, for the generator.
- systemd for bringing `can0` up.

**Spec:** `docs/superpowers/specs/2026-11-27-always-on-can-and-display-reader-design.md`

## Global Constraints

- **The display never transmits on the DAQ bus.** The socket is receive-only, and `can0` is brought up `listen-only on`.
- **Protocol header.** Include it as `#include "protocol/app_protocol.h"`, with the repository root (`..` from `driver-display/`) on the include path. Never copy its IDs.
- **Bit fields.** Decode them with explicit shifts and masks, never by casting to the packed structs.
- **Freshness.** A signal is stale after **500 ms** without a frame. A node is offline after **3000 ms** without any frame. Each node keeps its last **16** events.
- **Clocks.** Freshness, rates and "last seen" use the Pi's monotonic clock in ms (`uint64_t`). Node timestamps are only decoded and shown.
- **Dependencies.** No new ones on the Pi. It has `gcc`, `make` and Python 3.13, with no internet.
- **Pi frame loss is a known issue, not fixed here.** The plan only shows the Pi's own counters. Do not change `spimaxfrequency` or the hardware.
- **Stop and ask Lucas before:**
  - anything that transmits on `can0`;
  - any change to `protocol/app_protocol.h`;
  - adding a dependency.

## Review Focus

1. **Short or oversized frames.** A frame with a DAQ ID but the wrong length must decode as `DAQ_FRAME_MALFORMED`, never as garbage values. The test is in Task 1.
2. **Extended or remote frames that reuse DAQ ID numbers.** For example, extended `0x0B1` is not the front brake. It must decode as `DAQ_FRAME_UNKNOWN`. The test is in Task 1.
3. **A node reboots.** Its event sequence restarts at 0, which must not count as a gap. The rule matches the master: when diagnostics `resets` changes, forget the last sequence. The test is in Task 2.
4. **A value that stopped arriving.** It must read as not fresh at 500 ms, and a signal never seen is never fresh. The test is in Task 2.
5. **The generator pointed at the real bus.** `daq_sim.py` must refuse any interface whose name doesn't start with `vcan`, unless `--allow-real-interface` is given. The test is in Task 4.
6. **Replay of a log with health and GPS records.** The master's own health and GPS records must never be sent, and node status must go out at its 4-byte wire length, not the 8-byte logged one. The test is in Task 4.

---

### Task 1: Decoder for every DAQ frame

**Files:**
- Create: `driver-display/src/can/daq_decode.h`, `driver-display/src/can/daq_decode.c`
- Create: `driver-display/tests/test_daq.c`
- Modify: `driver-display/Makefile`. Add `-I..` to `CPPFLAGS`, and a `test` target that builds `build/test-daq` from `tests/test_daq.c src/can/daq_decode.c src/can/daq_state.c` with `$(CC) -std=gnu11 -Wall -Wextra -Werror` and runs it. `daq_state.c` arrives in Task 2, so list it in the target only from Task 2.

**Interfaces (produced, used by Tasks 2 to 4)**

```c
typedef struct {
    uint32_t id;        /* 11-bit, or 29-bit if extended */
    bool extended, remote;
    uint8_t len;        /* 0..8 */
    uint8_t data[8];
} daq_raw_frame_t;

typedef enum {
    DAQ_FRAME_UNKNOWN, DAQ_FRAME_MALFORMED,
    DAQ_FRAME_STOP, DAQ_FRAME_START, DAQ_FRAME_BEACON, DAQ_FRAME_NODE_STATE,
    DAQ_FRAME_SAMPLE, DAQ_FRAME_STATUS, DAQ_FRAME_EVENT, DAQ_FRAME_DIAGNOSTICS,
    DAQ_FRAME_IDENTITY, DAQ_FRAME_UPDATE_COMMAND, DAQ_FRAME_UPDATE_DATA,
    DAQ_FRAME_UPDATE_REPLY, DAQ_FRAME_AIM,
} daq_frame_kind_t;

typedef struct {
    daq_frame_kind_t kind;
    uint8_t node;   /* sender 1..8 for node frames, target for update commands, else 0 */
    union {
        struct { bool recording; uint64_t master_us; } beacon;
        struct { uint8_t state, reason, reset_reason; } node_state;
        struct { uint32_t id; int32_t value; uint32_t timestamp_ms; } sample;
        struct { uint32_t faults; uint8_t health, mode; uint8_t sensors[NODE_SENSOR_SLOTS]; } status;
        struct { uint8_t code, error, subject, sequence; uint16_t count; } event;
        struct { uint8_t tx_errors, rx_errors, resets, reset_reason; } diagnostics;
        struct { uint8_t elf_sha256[NODE_IDENTITY_HASH_LENGTH]; uint16_t protocol_hash; bool pending; } identity;
        struct { uint8_t op; } update_command;
        struct { uint8_t reply, error; } update_reply;
        struct { uint16_t rpm, counter; } aim;
    } u;
} daq_frame_t;

typedef struct { uint32_t id; const char *name; const char *unit; uint8_t node; } daq_signal_t;

daq_frame_t daq_decode(const daq_raw_frame_t *raw);
const daq_signal_t *daq_signals(size_t *count);   /* every sample ID, then AiM 0x500 */
int daq_signal_index(uint32_t id);                /* -1 if not a signal */
const char *daq_frame_kind_name(daq_frame_kind_t kind);
```

**Decoding rules**
- **Extended or remote frames** always decode as `UNKNOWN`.
- **Expected lengths:**

| Frame | Required length |
|---|---|
| sample | exactly 8 |
| beacon | exactly 8 |
| state | at least `CAN_NODE_STATE_LENGTH` |
| status, diagnostics | at least `NODE_HEALTH_PAYLOAD` (4) |
| event | exactly 8 |
| identity | exactly 8 |
| AiM | exactly 4 |
| Start, Stop | any length |

  A wrong length gives `MALFORMED`.
- **Node number:** `node` is `id - base`, only for nodes 1 to 8. Status, events and diagnostics also accept node 0, the master, which logs its own health but doesn't send it on the bus.
- **Signal names:**

| ID | Name and unit |
|---|---|
| `0x0B1` | `front brake`, `psi` |
| `0x0B2` | `rear brake`, `psi` |
| `0x0B3` to `0x0B5` | `accel X`, `accel Y`, `accel Z`, all `mg` |
| `0x0B6` to `0x0B8` | `gyro X`, `gyro Y`, `gyro Z`, all `mdeg/s` |
| `0x0B9` | `bearing RPM`, `rpm` |
| `0x0BA` | `generic ADC`, `mV` |
| `0x0BB` | `engine RPM`, `rpm` |
| `0x0BC` | `spark`, empty unit |
| `0x0BD`, `0x0BE` | `front pot 1`, `front pot 2`, `mm` |
| `0x0BF`, `0x0C9` | `rear pot 1`, `rear pot 2`, `mm` |
| `0x500` | `AiM RPM`, `rpm` |

  The node comes from `protocol_sensor_node_id()`, or 0 for AiM.

**Steps**
- [ ] **Step 1: Write the failing tests.** In `tests/test_daq.c`, write `main()` running `test_decode_*` functions with a `CHECK` macro that counts failures. `main` returns 1 if any failed.
  - **Front brake:** the golden frame `0x0B1` `DC 05 00 00 E8 03 00 00` gives `SAMPLE`, node 1, value 1500, timestamp 1000.
  - **Beacon:** `0x0A2` with `(1ULL<<63) | 12345678` gives `recording == true` and `master_us == 12345678`.
  - **State:** `0x0C3` `01 03 00` gives node 3, state 1, reason 3.
  - **Status, field by field:** build a `node_status_frame_t` with `faults = HEALTH_BIT(HEALTH_CAN_PASSIVE)`, `health = 2`, `mode = 1` and sensor slot 1 set to `SENSOR_STATE_NO_READINGS`. Send its first 4 bytes on `0x0D4`. Every decoded field must equal the struct's field.
  - **Event:** same idea with `node_health_pack_event`-style values set through the `health_event_frame_t` fields: code, error, subject, sequence 5 and count 300.
  - **Diagnostics:** `0x0F8` `12 34 02 01` gives node 8 with those four bytes.
  - **Identity:** `0x093` gives the hash bytes, the protocol hash read little-endian, and `pending` from `NODE_IDENTITY_PENDING`.
  - **AiM:** `0x500` `D0 07 05 00` gives rpm 2000 and counter 5.
  - **Malformed and unknown:**
    - `0x0B1` with length 7 gives `MALFORMED`;
    - `0x0B1` extended gives `UNKNOWN`;
    - `0x0B1` remote gives `UNKNOWN`;
    - `0x123` gives `UNKNOWN`.
  - **Signal table:** `daq_signal_index(0x0C9) >= 0`, `daq_signal_index(0x0C1) == -1`, and the table has 17 entries.
- [ ] **Step 2: Run the tests and check that they fail.**
  - Run: `make -C driver-display test`.
  - Expected: a compile error, because `daq_decode.h` doesn't exist yet.
- [ ] **Step 3: Implement `daq_decode.c`** following the rules above. Bit fields use shifts that follow the header's layout comments: status faults are bits 0 to 16, health 17 and 18, mode 19, sensors 20 to 31.
- [ ] **Step 4: Run the tests.**
  - Run: `make -C driver-display test`.
  - Expected: it prints `all tests passed` and exits 0.
- [ ] **Step 5: Commit.**

```bash
git add driver-display/src/can/daq_decode.* driver-display/tests/test_daq.c driver-display/Makefile
git commit -m "feat(display): decode every DAQ frame with the shared protocol header"
```

### Task 2: Live table of the bus

**Files:**
- Create: `driver-display/src/can/daq_state.h`, `driver-display/src/can/daq_state.c`
- Modify: `driver-display/tests/test_daq.c`, `driver-display/Makefile` (add `daq_state.c` to `test`)

**Interfaces (produced)**

```c
#define DAQ_STALE_MS 500
#define DAQ_NODE_OFFLINE_MS 3000
#define DAQ_EVENT_HISTORY 16

typedef struct { bool seen; int32_t value; uint32_t node_timestamp_ms; uint64_t last_ms;
                 uint32_t count, count_at_rate; float rate_hz; } daq_signal_state_t;

typedef struct {
    bool seen; uint64_t last_ms;                          /* any frame */
    bool state_seen; uint8_t state, reason, reset_reason;
    bool status_seen; uint32_t faults; uint8_t health, mode, sensors[NODE_SENSOR_SLOTS];
    bool diagnostics_seen; uint8_t tx_errors, rx_errors, resets, reset_reason_diag;
    bool identity_seen; uint8_t elf_sha256[NODE_IDENTITY_HASH_LENGTH]; uint16_t protocol_hash; bool pending;
    daq_frame_t events[DAQ_EVENT_HISTORY]; uint64_t event_ms[DAQ_EVENT_HISTORY];
    unsigned event_next, event_total;
    bool sequence_seen; uint8_t last_sequence; uint32_t sequence_gaps;
} daq_node_state_t;

typedef struct {
    bool beacon_seen; bool recording; uint64_t master_us; uint64_t beacon_ms;
    daq_frame_kind_t last_command; uint64_t command_ms;   /* START or STOP */
} daq_master_state_t;

typedef struct {
    daq_master_state_t master;
    daq_signal_state_t signals[DAQ_MAX_SIGNALS];   /* indexed like daq_signals() */
    daq_node_state_t nodes[CAN_NODE_SLOT_COUNT];   /* 1..8 used */
    uint32_t frames, unknown_frames, malformed_frames, update_frames;
    uint64_t rate_ms;                              /* when rates were last computed */
} daq_state_t;

void daq_state_init(daq_state_t *state);
void daq_state_feed(daq_state_t *state, const daq_raw_frame_t *raw, uint64_t now_ms);
void daq_state_update_rates(daq_state_t *state, uint64_t now_ms);  /* recomputes once >= 1000 ms passed */
bool daq_signal_fresh(const daq_state_t *state, int index, uint64_t now_ms);
bool daq_node_online(const daq_state_t *state, uint8_t node, uint64_t now_ms);
```

Also define `DAQ_MAX_SIGNALS 17` in `daq_decode.h`, and make the test check that it equals the table's count.

**Rules**
- A sample or AiM frame updates its signal and counts as a frame from its node, if the node is not 0.
- **Event gaps follow the master's `check_sequence`.** If `sequence_seen`, add `(sequence - last_sequence - 1) & HEALTH_EVENT_SEQUENCE_MASK` to `sequence_gaps`.
- A diagnostics frame whose `resets` differs from the stored one clears `sequence_seen`.
- Update frames only increase `update_frames`. Only update replies and identity frames mark their node as seen.

**Steps**
- [ ] **Step 1: Write the failing tests** `test_state_*`:
  - **Fresh then stale:** feed `0x0B1` at t=1000. Signal fresh at 1499, not fresh at 1500 (`now - last_ms >= DAQ_STALE_MS` means stale). A signal never fed is never fresh.
  - **Online then offline:** node 1 online at 3999, offline at 4000. Node 4, never seen, is offline.
  - **Rate:** 100 frames of `0x0B3` between t=0 and t=990, then `daq_state_update_rates(s, 1000)`. The rate is within 0.5 of 100 Hz.
  - **Event gaps:** events with sequences 0, 1, 4 give `sequence_gaps == 2`. Then diagnostics with `resets` changed, then an event with sequence 0, gives still 2.
  - **Event history:** feed 20 events. `event_total == 20`, and the newest is at `(event_next + 15) % 16`.
  - **Master:** a beacon with the recording bit sets `master.recording`. A Start frame sets `last_command = DAQ_FRAME_START`.
  - **Counters:** an unknown frame increases `unknown_frames`, and a malformed one increases `malformed_frames`.
- [ ] **Step 2: Run the tests and check that they fail.** Run `make -C driver-display test`. Expected: compile errors.
- [ ] **Step 3: Implement `daq_state.c`.**
- [ ] **Step 4: Run the tests.** Run `make -C driver-display test`. Expected: `all tests passed`.
- [ ] **Step 5: Commit.**

```bash
git add driver-display/src/can/daq_state.* driver-display/tests/test_daq.c driver-display/Makefile
git commit -m "feat(display): live table of DAQ signals, nodes and master"
```

### Task 3: SocketCAN reader and `can-monitor`

**Files:**
- Create: `driver-display/src/can/can_socket.h`, `driver-display/src/can/can_socket.c` (Linux only)
- Create: `driver-display/src/can_monitor.c`
- Modify: `driver-display/Makefile`. On Linux, `all` also builds `build/can-monitor` from `src/can_monitor.c` and `src/can/*.c`, with no LVGL.

**Interfaces (produced, used by step 2 later)**

```c
typedef enum { CAN_BUS_UNKNOWN, CAN_BUS_ACTIVE, CAN_BUS_WARNING, CAN_BUS_PASSIVE, CAN_BUS_OFF } can_bus_state_t;

typedef struct {
    int fd; char ifname[16];
    uint32_t socket_drops;          /* from SO_RXQ_OVFL */
    uint32_t error_frames;
    can_bus_state_t bus_state;      /* from controller and bus-off error frames */
} can_socket_t;

typedef struct { uint64_t rx_packets, rx_errors, rx_dropped, rx_over_errors;
                 uint32_t bitrate; bool listen_only; bool link_read; } can_link_t;

bool can_socket_open(can_socket_t *sock, const char *ifname);   /* raw, non-blocking, error frames on */
int  can_socket_drain(can_socket_t *sock, daq_state_t *state, uint64_t now_ms);  /* frames read, -1 on error */
void can_socket_read_link(const char *ifname, can_link_t *link);
void can_socket_close(can_socket_t *sock);
uint64_t can_monotonic_ms(void);   /* CLOCK_MONOTONIC */
```

**Implementation notes**
- **Socket:** never call `send` or `write` on it. Set `CAN_RAW_ERR_FILTER = CAN_ERR_MASK` and `SO_RXQ_OVFL`, and read with `recvmsg` to get the drop count from the control message. Error frames update `bus_state` and `error_frames`, and are not fed to the table.
- **`can_socket_read_link`:** counters come from `/sys/class/net/<if>/statistics/`. Bitrate and listen-only come from parsing `ip -details link show <if>` (`bitrate N`, `LISTEN-ONLY`) through `popen`.
- **`can-monitor [ifname] [--raw]`:** the default interface is `can0`. It drains every 10 ms. Without `--raw`, it clears the screen and redraws once per second. With `--raw`, it prints one line per decoded frame and no table. The redraw shows:
  - a header: interface, bitrate, listen-only, bus state, Pi drops (socket, overruns, RX errors);
  - the master: beacon age, recording, uptime, last command;
  - one line per node 1 to 8: online, state and reason, health, mode, TX and RX errors, resets, protocol hash, events total, sequence gaps (labelled as possibly Pi-side);
  - one line per signal: value, unit, rate, age in ms, or `NO DATA` when not fresh.

**Steps**
- [ ] **Step 1: Implement `can_socket.c` and `can_monitor.c`.** This is hardware glue, so it is checked on the Pi in Task 4. The pure logic is already covered by Tasks 1 and 2.
- [ ] **Step 2: Check that it builds on the Pi.**
  - Copy the tree with `./deploy.sh`, or `rsync` the `driver-display/` folder plus `protocol/` to the Pi.
  - Run on the Pi: `make -C ~/driver-display build/can-monitor`.
  - Expected: `Built build/can-monitor` with no warnings.
  - Check that `deploy.sh` also copies `../protocol/app_protocol.h`, and add it if it doesn't. The header lives outside `driver-display/`.
- [ ] **Step 3: Smoke test on the real interface.**
  - Run on the Pi: `./build/can-monitor can0` with `can0` up in listen-only mode at 500 kbit/s on the bench.
  - Expected: the header shows 500000, LISTEN-ONLY and bus state active. Every signal reads `NO DATA`, and every node is offline.
- [ ] **Step 4: Commit.**

```bash
git add driver-display/src/can/can_socket.* driver-display/src/can_monitor.c driver-display/Makefile driver-display/deploy.sh
git commit -m "feat(display): SocketCAN reader and can-monitor"
```

### Task 4: Traffic generator: replay a recording, or fake values, onto `vcan0`

**Files:**
- Create: `driver-display/tools/daq_sim.py` (Python 3 standard library only)
- Create: `driver-display/tools/test_daq_sim.py` (`unittest`, runs on any machine with no CAN)

**Interface:**

```text
python3 tools/daq_sim.py IFACE [--replay LOG.bin] [--speed X] [--loop]
                               [--nodes 1,3,4,5,7,8] [--no-master]
                               [--stop-node N --after S] [--allow-real-interface]
```

**The interface guard:** it refuses to start (exit 2, message `refusing to transmit on IFACE`) when IFACE does not start with `vcan`, unless `--allow-real-interface` is given.

**Replay mode (`--replay`)**
- The file is the master's binary log (see `docs/LOG_FORMAT.md`): 16-byte records with no header. Each record is a little-endian `u64` ID, then a `u64` payload (`value` is the low 32 bits, `timestamp_ms` the high 32). An incomplete trailing record is ignored.
- Each record becomes a wire frame:

| Logged ID | What it sends |
|---|---|
| Sample IDs (`protocol_is_sensor_id` set: `0x0B1` to `0x0BF`, `0x0C9`) | The 8 payload bytes, as is |
| Node status `0x0D1` to `0x0D8`, diagnostics `0x0F1` to `0x0F8` | The low 4 payload bytes only, length 4. The master added the high 4 bytes on arrival |
| Node events `0x0E1` to `0x0E8` | The 8 payload bytes, as is |
| `0x703` (AiM RPM record) | An AiM wire frame `0x500`: `u16 rpm`, `u16 counter`, where the counter increments per frame |
| Master health (`0x0D0`, `0x0E0`, `0x0F0`), GPS `0x700` to `0x702`, anything else | Nothing. They are never sent on the bus |

- **Pacing** follows `timestamp_ms`. A record is sent when `(timestamp_ms - first_timestamp) / speed` seconds have elapsed since replay started. Records are in arrival order, so a timestamp that goes backwards is sent right away.
- **Synthetic frames added alongside:**
  - a state report for each node seen in the file, sent once at start: active, reason `start`;
  - unless `--no-master`, a beacon every 100 ms of replay time, with the recording bit set and `master_us = replay timestamp × 1000`.
- `--loop` restarts at the end. Otherwise the generator exits when the file ends.

**Synthetic mode (no `--replay`)**
- **The master's beacon** every 100 ms (unless `--no-master`), with the recording bit off.
- **Each chosen node's samples** at the real rates. The values are plausible, and drive a slow "lap" cycle of about 60 s:

| Signal | Rate | Values |
|---|---|---|
| Engine RPM | per spark | 1800 to 3600, rising with the throttle phase. The spark (`0x0BC`, value 1) goes with each RPM sample, at RPM/60 per second |
| Bearing RPM | 50 Hz | Follows the engine through a variable ratio, 0 when braking |
| Brakes | 100 Hz | 0 psi, with pulses up to 1200 psi front and 900 psi rear in the braking phase |
| Accelerometer | 100 Hz | ±1500 mg, Z around 1000 mg, with a little noise |
| Gyroscope | 100 Hz | ±50000 mdeg/s |
| Linear pots | 100 Hz | 20 to 130 mm |

  When the engine is stopped (the first 3 s), node 5 sends RPM 0 every 100 ms.
- **Timestamps** are the generator's elapsed time in ms.
- **Status and diagnostics** at 1 Hz, OK health, idle mode.
- **One state report** at start: idle, reason `boot`.

**In both modes:** `--stop-node N --after S` silences node N after S seconds, to check the offline and `NO DATA` display.

**Frame building:** it lives in pure functions, so it can be tested without CAN:
- `record_to_frames(log_id, payload, counter) -> list[tuple[int, bytes]]`
- `read_records(path) -> list[tuple[int, int]]` (ID, payload)

**Steps**
- [ ] **Step 1: Write the failing tests** in `test_daq_sim.py`:
  - **Brake:** the `docs/LOG_FORMAT.md` front brake record (ID `B1 00 ..`, payload `DC 05 00 00 E8 03 00 00`) gives `[(0x0B1, bytes.fromhex('DC050000E8030000'))]`.
  - **Status:** a status record on `0x0D4` gives one frame of 4 bytes, equal to the payload's low 4 bytes.
  - **Event:** an event on `0x0E3` gives 8 bytes.
  - **AiM:** `0x703` with value 2000 and counter 5 gives `[(0x500, bytes([0xD0,0x07,0x05,0x00]))]`.
  - **Not on the bus:** `0x0D0`, `0x700` and `0x123` each give `[]`.
  - **File reading:** `read_records` on a 40-byte file returns 2 records and ignores the 8 trailing bytes.
  - **The guard:** running `daq_sim.py can0` as a subprocess exits with code 2 and prints `refusing to transmit on can0`.
- [ ] **Step 2: Run the tests and check that they fail.**
  - Run: `python3 -m unittest discover -s driver-display/tools -p 'test_*.py'`.
  - Expected: `ModuleNotFoundError: daq_sim`.
- [ ] **Step 3: Implement `daq_sim.py`.**
- [ ] **Step 4: Run the tests.** Same command. Expected: `OK`.
- [ ] **Step 5: Check on the Pi against `vcan0`.**

```bash
sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
python3 tools/daq_sim.py vcan0 --stop-node 4 --after 10 &
./build/can-monitor vcan0
```

  Expected:
  - nodes 1, 3, 4, 5, 7 and 8 online;
  - sample rates near 100 Hz, and 50 Hz for the bearing RPM;
  - the beacon age under 200 ms;
  - after 10 s, node 4 offline and the bearing RPM showing `NO DATA` within 0.5 s.

  Then replay a real recording copied from the SD card: `python3 tools/daq_sim.py vcan0 --replay log_0001.bin`. Expected: the values in `can-monitor` match the same samples in `decode_log.py`'s CSV of that file.
- [ ] **Step 6: Commit.**

```bash
git add driver-display/tools/daq_sim.py driver-display/tools/test_daq_sim.py
git commit -m "feat(display): DAQ traffic generator: replay a recording or fake values on vcan"
```

### Task 5: Bringing `can0` up, and the documentation

**Files:**
- Create: `driver-display/systemd/can0-listen.service.in`. It is a oneshot unit:

```text
ExecStartPre=-/sbin/ip link set can0 down
ExecStart=/sbin/ip link set can0 up type can bitrate @BITRATE@ listen-only on restart-ms 100
RemainAfterExit=yes
```

  It runs `After=sys-subsystem-net-devices-can0.device`.
- Modify: `driver-display/Makefile`. Add `can-on` (installs and enables the unit, with `BITRATE ?= 1000000`) and `can-off`, written like `autostart-on` and `autostart-off`.
- Modify: `driver-display/README.md`. Update the hardware row, "Open problems" 1 (solved), the code layout table, and a "Read the CAN bus" section on `can-monitor`, `make can-on BITRATE=500000` on the bench, and `make test`.
- Modify: `driver-display/docs/RASPBERRY_PI.md`. In "To set up for CAN", record the applied state: SPI on, the overlay with `oscillator=8000000`, the backup file, the 16 MHz crystal still needed.
- Add to git: `driver-display/hardware/` (README, schematic files) and `driver-display/tools/can_check.py`, which were untracked until now. In the CAN module README, add the known Pi frame-loss limitation from the spec.

**Steps**
- [ ] **Step 1: Write the unit and the Makefile targets.**
- [ ] **Step 2: Check them on the Pi.**
  - Run: `make can-on BITRATE=500000`, then `ip -details link show can0`.
  - Expected: `state UP`, `LISTEN-ONLY`, `bitrate 500000`.
  - After `sudo reboot`, the same output appears without running anything.
- [ ] **Step 3: Update the documentation** as listed in Files.
- [ ] **Step 4: Run the host tests one last time.** Run `make -C driver-display test`. Expected: `all tests passed`.
- [ ] **Step 5: Commit.**

```bash
git add driver-display
git commit -m "feat(display): bring can0 up listen-only at boot, document the CAN reader"
```

- [ ] **Step 6: Hand the real-bus checks to Lucas.** They need the 16 MHz crystal, so report them as not yet run:
  - `oscillator=16000000`;
  - `make can-on` at 1 Mbit/s;
  - `can-monitor can0` on the car matches the web page;
  - no CAN errors appear on the master;
  - Pi drops observed.
