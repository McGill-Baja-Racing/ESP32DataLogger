# Nodes Always Send: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** sensor nodes sample and send on CAN from boot, whether or not the master records. Start/Stop only controls SD logging, and the master's liveness tracking and live view work all the time.

**Architecture:**
- **Nodes.** The sampler starts once at boot. Start/Stop and the beacon only change the node's recording state, which gates health events and the status mode bit. The sampler pauses only during an update over CAN.
- **Master.** It stops toggling monitoring and live data with recording, and drops the health counts made while idle when Start arrives. CAN dispatch moves off the lifecycle mutex onto a short processing mutex, so an unnamed Start's GPS wait no longer stalls reception.

**Tech Stack:**
- ESP-IDF on PlatformIO, C, for both firmwares.
- Host tests in Python `unittest`: each test cuts C source out of the firmware and compiles it with `cc` against stubs.

**Spec:** `docs/superpowers/specs/2026-11-27-always-on-can-and-display-reader-design.md`

## Global Constraints

- **Protocol header.** No code change in `protocol/app_protocol.h`. Comments only, because the protocol hash ignores comments.
- **Wire and log formats.** No change to CAN IDs, rates, health frame layouts or log file formats.
- **SD admission.** It stays as today: samples are admitted only in `LOGGER_RUNNING`, and health admission keeps its 200 ms grace after Stop.
- **Dispatch.** Frame dispatch must never wait on Start or Stop for more than about 50 ms.
- **Mixed firmware.** A node on old firmware must keep working. It stays silent until Start.
- **Host tests.** Run them from the repository root with `python3 -m unittest discover -s master-node/tests`. They need `cc` and Python 3.9 or later.
- **Builds.** All builds must compile:
  - master: `Master`, `MasterStable`;
  - nodes: `NodeBrake`, `NodeMPU`, `NodeEncoder`, `NodeEngine`, `NodeEngineBench`, `NodeADC`, `NodeFrontShock`, `NodeRearShock`.
- **Stop and ask Lucas before:**
  - any change to the protocol header beyond comments;
  - changing a log format, CAN ID, rate or health layout;
  - changing what Start/Stop means for the SD log;
  - adding a dependency.

## Review Focus

1. **Start arrives only through the beacon.** If the node misses every Start frame and follows the beacon's recording bit, its idle counts must still be dropped. `on_start` runs on both paths in `can_node.c`, so check that the new call sits in `on_start`, not in the Start-frame branch.
2. **Health counts never move backwards.** A baseline taken at Start and applied a little later must not lower an already-reported count, which would resend events. The test is in Task 1.
3. **An unnamed Start while frames arrive.** The GPS wait in `data_logger_prepare` must run without the processing lock held. The test is in Task 3.
4. **A node reboots mid-recording.** It must sample right away, and send its boot event while the master is recording. The boot event is never dropped by the idle reset (Task 1).
5. **An update over CAN that aborts or times out.** The node must resume sampling. Check every path in Task 2: `fail()`, ABORT, a new BEGIN during an update, and the timeout.

---

### Task 1: Shared helper that drops idle health counts

**Files:**
- Modify: `protocol/health_counters.h`
- Modify: `protocol/health_counters.c`
- Test: `master-node/tests/test_node_health.py`

**Interfaces:**
- Produces: `void node_health_skip_counts(health_state_t *reported, const health_state_t *baseline);`
  - For each counter `i` whose `baseline->counters[i].code != HEALTH_BOOT`, it sets `reported->counters[i].count = baseline->counters[i].count`.
  - It does this only when the baseline is ahead: `(int32_t)(baseline - reported) > 0`.
  - It never touches `active[]`.

- [ ] **Step 1: Write the failing test.** In the C `HARNESS` of `test_node_health.py`, add `static void test_skip_counts(void)` and call it from `main()`:

```c
static void test_skip_counts(void)
{
    health_state_t baseline, reported;
    node_health_count(HEALTH_SAMPLES_MISSED, 5, 0x0B1, ESP_OK);
    node_health_copy_counts(&baseline);
    memset(&reported, 0, sizeof(reported));
    node_health_skip_counts(&reported, &baseline);
    for (size_t i = 0; i < HEALTH_COUNTER_COUNT; i++) {
        uint32_t expected = baseline.counters[i].code == HEALTH_BOOT ? 0 : baseline.counters[i].count;
        CHECK(reported.counters[i].count == expected);
    }
    /* Never backwards: a count reported after the baseline was taken stays. */
    size_t k = 0;
    while (baseline.counters[k].code != HEALTH_SAMPLES_MISSED) k++;
    reported.counters[k].count = baseline.counters[k].count + 3;
    node_health_skip_counts(&reported, &baseline);
    CHECK(reported.counters[k].count == baseline.counters[k].count + 3);
}
```

- [ ] **Step 2: Run the test and check that it fails.**
  - Run: `python3 -m unittest master-node.tests.test_node_health` from the repository root, or `discover` as in Global Constraints.
  - Expected: a compile error, because `node_health_skip_counts` is undeclared.
- [ ] **Step 3: Add `node_health_skip_counts`.** Declare it in `health_counters.h`, with a doc comment saying it is used at Start so events only describe the recording. Define it in `health_counters.c`.
- [ ] **Step 4: Run all host tests.** Expected: everything passes.
- [ ] **Step 5: Commit.**

```bash
git add protocol/health_counters.h protocol/health_counters.c master-node/tests/test_node_health.py
git commit -m "feat(health): drop counts made before a recording"
```

### Task 2: Sensor nodes sample from boot

**Files:**
- Modify: `sensor-node/src/main.c` (`handle_start`, `handle_stop` and the `app_main` CAN branch, plus the comment above the callbacks)
- Modify: `sensor-node/src/health/node_health.h`
- Modify: `sensor-node/src/health/node_health.c` (`send_event` gate at line 141, `health_task` at lines 238 to 290)
- Modify: `sensor-node/src/update/firmware_update.c` (`stop_receiving`, `begin_update`)
- Modify: `sensor-node/src/sampler/sampler.h`, `sensor-node/src/can/can_node.h`: doc comments only

**Interfaces:**
- Consumes: `node_health_skip_counts()` from Task 1.
- Produces: `void node_health_recording_started(void);`. It is safe from the CAN receive task. It snapshots the current counts (`node_health_copy_counts`) and every sensor's `failed_reads` into a static baseline under `lock`, and marks the baseline pending.

**Steps**
- [ ] **Step 1: Add `node_health_recording_started()` in `node_health.c`.**
  - In `health_task`, right after `now` is copied and before anything is sent: if a baseline is pending, call `node_health_skip_counts(&reported.counts, &baseline.counts)`. Then advance each `reported.sensors[i].failed_reads` to the baseline's value with the same wrap-safe "only forward" rule, and clear the pending flag. Do both under `lock`.
  - `init_reported[]` is left alone, so a sensor init failure is still sent once.
- [ ] **Step 2: Gate on the recording state.**
  - `send_event` sends only if `can_node_state() == NODE_STATE_ACTIVE && time_sync_is_current(TIME_SYNC_TIMEOUT_MS)`.
  - The status `mode` comes from `can_node_state() == NODE_STATE_ACTIVE`.
  - Remove the `sampler/sampler.h` include if nothing else uses it.
  - Update the file's header comment: counts made while idle are dropped at Start.
- [ ] **Step 3: Change `main.c`.**
  - `handle_start` calls `node_health_recording_started()`. Delete `handle_stop` and set `.on_stop = NULL`.
  - Call `sampler_start()` right after `node_health_init()`, before the loop that waits for the beacon. A node with no master must still sample.
  - Rewrite the comment that says nodes wait for Start.
- [ ] **Step 4: Pause sampling during updates in `firmware_update.c`.**
  - `begin_update` calls `sampler_stop()` right after `receiving = true`.
  - `stop_receiving` calls `sampler_start()` when it clears `receiving`.
  - Include `sampler/sampler.h`.
  - A successful update reboots, so it needs no resume.
- [ ] **Step 5: Update the doc comments.**
  - In `sampler.h`, start and stop mean "sampling on and off": once at boot, and around updates. `sampler_is_active` means "sampling".
  - In `can_node.h`, `on_start` and `on_stop` mean "the master's recording started or stopped", whether through a Start or Stop frame or the beacon.
- [ ] **Step 6: Build every node profile.**
  - Run from `sensor-node/`: `pio run -e NodeBrake -e NodeMPU -e NodeEncoder -e NodeEngine -e NodeEngineBench -e NodeADC -e NodeFrontShock -e NodeRearShock`.
  - Expected: every environment reports `SUCCESS`.
- [ ] **Step 7: Run the host tests.** Expected: everything passes.
- [ ] **Step 8: Commit.**

```bash
git add sensor-node/src
git commit -m "feat(node): sample and send from boot, not only while recording"
```

### Task 3: Master Start/Stop no longer blocks CAN, and drops idle counts

**Files:**
- Modify: `master-node/src/app/app_control.c` (`app_control_init`, `app_control_start_logging`, `app_control_stop_logging`, `app_control_start_live_data`, the lock and unlock functions at lines 194 to 202)
- Modify: `master-node/src/app/app_control.h`: comments of the lock functions
- Modify: `master-node/src/can/can_master.c` and `.h`: delete `can_master_clear_receive_queue`
- Modify: `master-node/src/health/node_health.c` and `.h`
- Modify: `master-node/src/live/live_data.c` and `.h`: delete `live_data_force_stop`
- Modify: `master-node/src/web/web_server.c`: delete the 409 branch of `live_start_handler`
- Test: `master-node/tests/test_recording_lifecycle.py`
- Test: `master-node/tests/test_aim.py`

**Interfaces:**
- Consumes: `node_health_skip_counts()` from Task 1.
- Produces in the master's `node_health.h`: `void node_health_recording_started(void);`. It works like the node version: it snapshots counts into a baseline, and the health task applies it before `log_events`.
- Produces: `app_control_lock_can_processing()` and `app_control_unlock_can_processing()` now take a new `processing_mutex`, created in `app_control_init()`. The signatures don't change.

**New order of `app_control_start_logging`**
1. Under `lifecycle_mutex`, as today: the checks, then `data_logger_prepare(filename)`. The GPS wait happens here, **without** `processing_mutex`.
2. Take `processing_mutex`, then call `data_logger_clear_queue()`, `node_health_recording_started()`, `data_logger_start()` and `time_beacon_set_recording(true)`. Give `processing_mutex` back.
3. Call `can_master_start_nodes()`. Give `lifecycle_mutex` back.

It no longer calls `can_master_clear_receive_queue` or `node_registry_set_monitoring`.

**New order of `app_control_stop_logging`**
1. Under `lifecycle_mutex`, check the logger state.
2. Under `processing_mutex`: `data_logger_stop()`, then `time_beacon_set_recording(false)`.
3. Call `can_master_stop_nodes()`.

It no longer calls `live_data_force_stop` or `node_registry_set_monitoring`.

**`app_control_start_live_data`.** The `LOGGER_RUNNING` check goes.

**Steps**
- [ ] **Step 1: Rewrite the lifecycle test harness** in `test_recording_lifecycle.py::test_start_stop_boundaries` for two mutexes.
  - The handles are `lifecycle_mutex=1` and `processing_mutex=2`. `xSemaphoreTake` and `xSemaphoreGive` track `locked[2]` and assert the handle is valid.
  - Delete the stubs for `can_master_clear_receive_queue`, `node_registry_set_monitoring` and `live_data_force_stop`. A call left in the code then fails to compile.
  - Add a stub for `node_health_recording_started` that asserts the processing lock is held and the state is `LOGGER_IDLE`, and counts its calls.
  - Assert in the stubs:
    - `data_logger_prepare`: `locked[LIFECYCLE] && !locked[PROCESSING]`.
    - `data_logger_clear_queue`, `data_logger_start` and `time_beacon_set_recording`: `locked[PROCESSING]`.
    - `can_master_start_nodes` and `can_master_stop_nodes`: `locked[LIFECYCLE] && !locked[PROCESSING]`.
  - In `main`:
    - `node_health_recording_started` is called once per successful Start, and never when `prepare` fails;
    - Stop leaves the log queue pending (drain, don't clear), as today.
- [ ] **Step 2: Add `test_live_data_starts_while_idle`** to the same file. Cut `app_control_start_live_data` with stubs where `data_logger_state()` returns `LOGGER_IDLE` and `live_data_start` returns `LIVE_DATA_OK`. Assert the result is `APP_CONTROL_OK`.
- [ ] **Step 3: Update `test_aim.py`.**
  - Cut the dispatch from `/* Keep dequeue` up to `static void dispatch_task(`, since `can_master_clear_receive_queue` is gone.
  - Drop the two blocks that clear the queue at Start.
  - Assert that idle dispatch gives `logged==0 && live==1`: AiM reaches live data while idle.
  - Assert that dispatch while recording gives `logged` and `live` both increasing.
- [ ] **Step 4: Run the tests and check that they fail.** Expected: compile errors, or assertion failures on the old ordering.
- [ ] **Step 5: Implement the new orders above.**
  - In `master-node/src/main.c::handle_can_message`, the AiM branch becomes:
    - `live_data_record(&sample)` whenever `aim_adapter_receive` succeeds;
    - `data_logger_enqueue(&sample)` only when `data_logger_state() == LOGGER_RUNNING`.
  - In the master's `node_health.c`, the health task applies a pending baseline with `node_health_skip_counts(&reported, &baseline)` before `if (recording) log_events(...)`.
  - Delete the functions listed in Files.
- [ ] **Step 6: Run the host tests.** Expected: everything passes.
- [ ] **Step 7: Build the master.**
  - Run from `master-node/`: `pio run -e Master -e MasterStable`.
  - Expected: `SUCCESS`.
- [ ] **Step 8: Commit.**

```bash
git add master-node/src master-node/tests
git commit -m "feat(master): keep CAN dispatch off Start/Stop and drop idle health counts"
```

### Task 4: Master tracks nodes and serves live data all the time

**Files:**
- Modify: `master-node/src/node_state/node_registry.c` and `.h`
- Modify: `master-node/src/web/index.html`, lines 41, 107, 109 and 159
- Create: `master-node/tests/test_node_registry.py`

**Interfaces:**
- Removes `node_registry_set_monitoring`. Its last callers went in Task 3.
- `node_registry_init()` now sets `last_seen = xTaskGetTickCount()` for every configured node before it starts `monitor_task`.
- `node_registry_state_name()` returns `"disabled"`, `"offline"`, `"waiting"` or `"active"`. It never returns `"off"`.

**Steps**
- [ ] **Step 1: Write `test_node_registry.py`.**
  - Cut `node_registry.c` from its first function to `static void monitor_task(`, plus `node_registry_state_name`.
  - Stub `xTaskGetTickCount` with a settable tick, `pdMS_TO_TICKS(ms)` as `ms`, the critical sections as no-ops, and `ESP_LOGI` and `ESP_LOGW` as no-ops.
  - Assert:
    - with no Start ever called, `node_registry_record_sensor_frame` for `0x0B1` makes node 1 `"active"`;
    - 3001 ms later with no frame, node 1 is `"waiting"`;
    - a health frame only, for node 3, gives `"waiting"`;
    - no configured node is ever `"off"`.
- [ ] **Step 2: Run the test and check that it fails.** Expected: `"off"` is returned, because monitoring is false at boot.
- [ ] **Step 3: Implement.**
  - Delete the `monitoring` flag and every check of it.
  - Delete `node_registry_set_monitoring` from `.c` and `.h`, and update the header comments that say "only while monitoring".
  - Set `last_seen` in `node_registry_init`.
- [ ] **Step 4: Change the web page.**
  - Line 41: `Live viewing adds Wi-Fi and browser load. It works whether or not a recording is running.`
  - Line 107: `$('liveEnable').disabled=token!==null||s.live_enabled;`
  - Line 109: `if(token&&!s.live_enabled) endLive(false,'Live data stopped.');`
  - Line 159: `$('liveEnable').disabled=false;`
- [ ] **Step 5: Run the host tests.** Expected: everything passes.
- [ ] **Step 6: Build the master** with `pio run -e Master -e MasterStable`. Expected: `SUCCESS`.
- [ ] **Step 7: Commit.**

```bash
git add master-node/src master-node/tests/test_node_registry.py
git commit -m "feat(master): monitor nodes and serve live data outside recordings"
```

### Task 5: Documentation and the final check

**Files:**
- Modify: `docs/PROTOCOL.md`, `docs/SYSTEM_OVERVIEW.md`, `docs/TESTING.md`, `docs/TROUBLESHOOTING.md`, `docs/WEB_TUTORIAL.md`
- Modify: `sensor-node/README.md`, `sensor-node/docs/NODE_ARCHITECTURE.md`, `sensor-node/docs/ADDING_SENSORS_AND_NODES.md`
- Modify: `master-node/README.md`, `master-node/docs/MASTER_ARCHITECTURE.md`, `master-node/docs/AIM_EVO4S.md`, `master-node/src/logger/README.md`
- Modify: comments in `protocol/app_protocol.h` only: the health block says events are sent while recording, and idle counts are dropped at Start

**Steps**
- [ ] **Step 1: Rewrite each place that says nodes sample or send only after Start.** The places to find:
  - `PROTOCOL.md` lines 16, 25, 45, 100 and 101, 115 and 116;
  - `sensor-node/README.md:30`;
  - `NODE_ARCHITECTURE.md` lines 13, 19, 25 and 29;
  - `ADDING_SENSORS_AND_NODES.md:467`;
  - `MASTER_ARCHITECTURE.md` lines 64 to 66 and 74;
  - `master-node/src/logger/README.md` lines 9 to 11;
  - `master-node/README.md:11`;
  - `SYSTEM_OVERVIEW.md` lines 21 to 23;
  - `TESTING.md:67`;
  - `TROUBLESHOOTING.md:12`;
  - `WEB_TUTORIAL.md:44`;
  - `AIM_EVO4S.md:27`.

  Each must say what the spec decides:
  - nodes always sample and send;
  - the state byte and the mode bit mean "the master is recording";
  - Start/Stop controls the SD log;
  - idle health counts are dropped at Start;
  - updates pause sampling.
- [ ] **Step 2: Check that the protocol hash didn't change.**
  - Run: `git diff main -- protocol/app_protocol.h | grep '^[+-]' | grep -v '^[+-][+-]' | grep -vE '^[+-]\s*(/\*|\*|//)'`.
  - Expected: no output. Only comment lines changed.
- [ ] **Step 3: Run the host tests and every build from Global Constraints.** Expected: everything passes and all 10 environments report `SUCCESS`.
- [ ] **Step 4: Commit.**

```bash
git add docs sensor-node master-node protocol/app_protocol.h
git commit -m "docs: nodes always send; Start/Stop only controls the SD log"
```

- [ ] **Step 5: Hand the bench checks to Lucas.** These need hardware, so report them as not yet run:
  - nodes send from power-up with the master off;
  - the web page shows live values and online nodes while idle;
  - a recording is complete, with no `HEALTH_EVENT_LOST` and no RX overflow at Start;
  - an update over CAN of an idle node works while the other nodes keep sending;
  - a mix of old and new firmware works.
