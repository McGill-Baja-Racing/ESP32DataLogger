/* Host tests for the DAQ decoder and live table. Run with `make test`. */
#include <stdio.h>
#include <string.h>

#include "can/daq_decode.h"
#include "can/daq_state.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; } } while (0)

static daq_raw_frame_t frame(uint32_t id, const uint8_t *bytes, uint8_t len)
{
    daq_raw_frame_t raw = {.id = id, .len = len};
    memcpy(raw.data, bytes, len);
    return raw;
}

static void test_decode_brake_sample(void)
{
    /* The worked example of docs/PROTOCOL.md: 1500 psi at 1000 ms. */
    const uint8_t bytes[] = {0xDC, 0x05, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00};
    daq_raw_frame_t raw = frame(CAN_ID_FRONT_BRAKE, bytes, 8);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_SAMPLE);
    CHECK(decoded.node == 1);
    CHECK(decoded.u.sample.id == CAN_ID_FRONT_BRAKE);
    CHECK(decoded.u.sample.value == 1500);
    CHECK(decoded.u.sample.timestamp_ms == 1000);
}

static void test_decode_negative_sample(void)
{
    const uint8_t bytes[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00};
    daq_raw_frame_t raw = frame(CAN_ID_BEARING_ENCODER, bytes, 8);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_SAMPLE && decoded.node == 4);
    CHECK(decoded.u.sample.value == -1);
}

static void test_decode_beacon(void)
{
    uint64_t value = CAN_MASTER_TIME_RECORDING_FLAG | 12345678u;
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    daq_raw_frame_t raw = frame(CAN_ID_MASTER_TIME, bytes, 8);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_BEACON);
    CHECK(decoded.u.beacon.recording);
    CHECK(decoded.u.beacon.master_us == 12345678u);
}

static void test_decode_start_stop(void)
{
    daq_raw_frame_t start = frame(CAN_ID_START, NULL, 0);
    daq_raw_frame_t stop = frame(CAN_ID_STOP, (const uint8_t[]){0xFF}, 1);
    CHECK(daq_decode(&start).kind == DAQ_FRAME_START);
    CHECK(daq_decode(&stop).kind == DAQ_FRAME_STOP);
}

static void test_decode_node_state(void)
{
    const uint8_t bytes[] = {1, 3, 0};
    daq_raw_frame_t raw = frame(CAN_ID_NODE_STATE_BASE + 3, bytes, 3);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_NODE_STATE && decoded.node == 3);
    CHECK(decoded.u.node_state.state == 1 && decoded.u.node_state.reason == 3);
    CHECK(decoded.u.node_state.reset_reason == 0);
}

static void test_decode_status_matches_struct(void)
{
    node_status_frame_t status = {
        .faults = HEALTH_BIT(HEALTH_CAN_PASSIVE) | HEALTH_BIT(HEALTH_SAMPLES_MISSED),
        .health = NODE_HEALTH_ERROR,
        .mode = NODE_MODE_RECORDING,
        .sensors = (uint32_t)SENSOR_STATE_NO_READINGS << 2 | (uint32_t)SENSOR_STATE_INIT_FAILED << 10,
    };
    daq_raw_frame_t raw = frame(CAN_ID_NODE_STATUS_BASE + 4, (const uint8_t *)&status, NODE_HEALTH_PAYLOAD);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_STATUS && decoded.node == 4);
    CHECK(decoded.u.status.faults == status.faults);
    CHECK(decoded.u.status.health == status.health);
    CHECK(decoded.u.status.mode == status.mode);
    for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS; slot++) {
        CHECK(decoded.u.status.sensors[slot] == node_status_sensor_state(&status, slot));
    }
}

static void test_decode_event_matches_struct(void)
{
    health_event_frame_t event = {
        .code = HEALTH_SENSOR_READ, .error = HEALTH_ERROR_TIMEOUT, .subject = 0xB1,
        .sequence = 5, .count = 300, .timestamp_ms = 4242,
    };
    daq_raw_frame_t raw = frame(CAN_ID_NODE_EVENT_BASE + 1, (const uint8_t *)&event, 8);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_EVENT && decoded.node == 1);
    CHECK(decoded.u.event.code == event.code);
    CHECK(decoded.u.event.error == event.error);
    CHECK(decoded.u.event.subject == event.subject);
    CHECK(decoded.u.event.sequence == event.sequence);
    CHECK(decoded.u.event.count == event.count);
    CHECK(decoded.u.event.timestamp_ms == event.timestamp_ms);
}

static void test_decode_diagnostics(void)
{
    const uint8_t bytes[] = {0x12, 0x34, 0x02, 0x01};
    daq_raw_frame_t raw = frame(CAN_ID_NODE_DIAGNOSTICS_BASE + 8, bytes, 4);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_DIAGNOSTICS && decoded.node == 8);
    CHECK(decoded.u.diagnostics.tx_errors == 0x12 && decoded.u.diagnostics.rx_errors == 0x34);
    CHECK(decoded.u.diagnostics.resets == 2 && decoded.u.diagnostics.reset_reason == 1);
}

static void test_decode_identity(void)
{
    node_identity_frame_t identity = {
        .elf_sha256 = {1, 2, 3, 4, 5}, .protocol_hash = 0xFBD3, .flags = NODE_IDENTITY_PENDING,
    };
    daq_raw_frame_t raw = frame(CAN_ID_NODE_IDENTITY_BASE + 3, (const uint8_t *)&identity, 8);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_IDENTITY && decoded.node == 3);
    CHECK(memcmp(decoded.u.identity.elf_sha256, identity.elf_sha256, NODE_IDENTITY_HASH_LENGTH) == 0);
    CHECK(decoded.u.identity.protocol_hash == 0xFBD3);
    CHECK(decoded.u.identity.pending);
}

static void test_decode_update_frames(void)
{
    daq_raw_frame_t command = frame(CAN_ID_UPDATE_COMMAND, (const uint8_t[]){5, UPDATE_OP_BEGIN, 0, 1, 0, 0}, 6);
    daq_frame_t decoded = daq_decode(&command);
    CHECK(decoded.kind == DAQ_FRAME_UPDATE_COMMAND && decoded.node == 5);
    CHECK(decoded.u.update_command.op == UPDATE_OP_BEGIN);
    daq_raw_frame_t data = frame(CAN_ID_UPDATE_DATA, (const uint8_t[]){0x21, 1, 2}, 3);
    CHECK(daq_decode(&data).kind == DAQ_FRAME_UPDATE_DATA);
    daq_raw_frame_t reply = frame(CAN_ID_NODE_UPDATE_BASE + 7, (const uint8_t[]){UPDATE_REPLY_ERROR, UPDATE_ERROR_FLASH}, 2);
    decoded = daq_decode(&reply);
    CHECK(decoded.kind == DAQ_FRAME_UPDATE_REPLY && decoded.node == 7);
    CHECK(decoded.u.update_reply.reply == UPDATE_REPLY_ERROR && decoded.u.update_reply.error == UPDATE_ERROR_FLASH);
}

static void test_decode_aim(void)
{
    const uint8_t bytes[] = {0xD0, 0x07, 0x05, 0x00};
    daq_raw_frame_t raw = frame(CAN_ID_AIM_WIRE, bytes, 4);
    daq_frame_t decoded = daq_decode(&raw);
    CHECK(decoded.kind == DAQ_FRAME_AIM && decoded.node == 0);
    CHECK(decoded.u.aim.rpm == 2000 && decoded.u.aim.counter == 5);
}

static void test_decode_malformed_and_unknown(void)
{
    const uint8_t bytes[8] = {0};
    daq_raw_frame_t short_sample = frame(CAN_ID_FRONT_BRAKE, bytes, 7);
    CHECK(daq_decode(&short_sample).kind == DAQ_FRAME_MALFORMED);
    daq_raw_frame_t short_event = frame(CAN_ID_NODE_EVENT_BASE + 1, bytes, 4);
    CHECK(daq_decode(&short_event).kind == DAQ_FRAME_MALFORMED);
    daq_raw_frame_t short_beacon = frame(CAN_ID_MASTER_TIME, bytes, 4);
    CHECK(daq_decode(&short_beacon).kind == DAQ_FRAME_MALFORMED);
    daq_raw_frame_t extended = frame(CAN_ID_FRONT_BRAKE, bytes, 8);
    extended.extended = true;
    CHECK(daq_decode(&extended).kind == DAQ_FRAME_UNKNOWN);
    daq_raw_frame_t remote = frame(CAN_ID_FRONT_BRAKE, bytes, 8);
    remote.remote = true;
    CHECK(daq_decode(&remote).kind == DAQ_FRAME_UNKNOWN);
    daq_raw_frame_t other = frame(0x123, bytes, 8);
    CHECK(daq_decode(&other).kind == DAQ_FRAME_UNKNOWN);
    /* Node 9 is not a node: 0x0C9 is the rear pot, 0x0D9 is nothing. */
    daq_raw_frame_t no_node = frame(CAN_ID_NODE_STATUS_BASE + 9, bytes, 4);
    CHECK(daq_decode(&no_node).kind == DAQ_FRAME_UNKNOWN);
}

static void test_signal_table(void)
{
    size_t count = 0;
    const daq_signal_t *signals = daq_signals(&count);
    CHECK(count == DAQ_MAX_SIGNALS);
    CHECK(count == 17);
    CHECK(daq_signal_index(CAN_ID_REAR_LIN_2) >= 0);
    CHECK(daq_signal_index(CAN_ID_NODE_STATE_BASE + 1) == -1);
    int brake = daq_signal_index(CAN_ID_FRONT_BRAKE);
    CHECK(brake >= 0 && !strcmp(signals[brake].unit, "psi") && signals[brake].node == 1);
    int aim = daq_signal_index(CAN_ID_AIM_WIRE);
    CHECK(aim >= 0 && signals[aim].node == 0);
    for (size_t i = 0; i < count; i++) {
        CHECK(signals[i].name && signals[i].name[0]);
        CHECK(daq_signal_index(signals[i].id) == (int)i);
    }
}

/* ---- live table ---- */

static daq_raw_frame_t sample_frame(uint32_t id, int32_t value)
{
    uint8_t bytes[8] = {0};
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)((uint32_t)value >> (8 * i));
    return frame(id, bytes, 8);
}

static daq_raw_frame_t event_frame(uint8_t node, uint8_t sequence)
{
    health_event_frame_t event = {.code = HEALTH_SAMPLES_MISSED, .sequence = sequence, .count = 1};
    return frame(CAN_ID_NODE_EVENT_BASE + node, (const uint8_t *)&event, 8);
}

static daq_raw_frame_t diagnostics_frame(uint8_t node, uint8_t resets)
{
    return frame(CAN_ID_NODE_DIAGNOSTICS_BASE + node, (const uint8_t[]){0, 0, resets, 1}, 4);
}

static void test_state_fresh_then_stale(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    int brake = daq_signal_index(CAN_ID_FRONT_BRAKE);
    int rear = daq_signal_index(CAN_ID_REAR_BRAKE);
    daq_raw_frame_t raw = sample_frame(CAN_ID_FRONT_BRAKE, 1500);
    daq_state_feed(&state, &raw, 1000);
    CHECK(state.signals[brake].seen && state.signals[brake].value == 1500);
    CHECK(daq_signal_fresh(&state, brake, 1499));
    CHECK(!daq_signal_fresh(&state, brake, 1000 + DAQ_STALE_MS));
    CHECK(!daq_signal_fresh(&state, rear, 0));        /* never seen is never fresh */
    CHECK(!daq_signal_fresh(&state, -1, 1000));
}

static void test_state_online_then_offline(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    daq_raw_frame_t raw = sample_frame(CAN_ID_FRONT_BRAKE, 1);
    daq_state_feed(&state, &raw, 1000);
    CHECK(daq_node_online(&state, 1, 3999));
    CHECK(!daq_node_online(&state, 1, 1000 + DAQ_NODE_OFFLINE_MS));
    CHECK(!daq_node_online(&state, 4, 1000));       /* never seen */
    CHECK(!daq_node_online(&state, 0, 1000) && !daq_node_online(&state, 9, 1000));
    daq_raw_frame_t status = frame(CAN_ID_NODE_STATUS_BASE + 3, (const uint8_t[]){0, 0, 0, 0}, 4);
    daq_state_feed(&state, &status, 2000);         /* health alone keeps a node online */
    CHECK(daq_node_online(&state, 3, 2000) && state.nodes[3].status_seen);
}

static void test_state_rate(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    int accel = daq_signal_index(CAN_ID_MPU_ACCEL_X);
    daq_state_update_rates(&state, 0);
    for (int i = 0; i < 100; i++) {
        daq_raw_frame_t raw = sample_frame(CAN_ID_MPU_ACCEL_X, i);
        daq_state_feed(&state, &raw, (uint64_t)i * 10);
    }
    daq_state_update_rates(&state, 1000);
    CHECK(state.signals[accel].rate_hz > 99.5f && state.signals[accel].rate_hz < 100.5f);
    daq_state_update_rates(&state, 1500);          /* under a second: unchanged */
    CHECK(state.signals[accel].rate_hz > 99.5f);
    daq_state_update_rates(&state, 2000);          /* a silent second */
    CHECK(state.signals[accel].rate_hz == 0.0f);
}

static void test_state_event_gaps(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    const uint8_t sequences[] = {0, 1, 4};
    for (int i = 0; i < 3; i++) {
        daq_raw_frame_t raw = event_frame(2, sequences[i]);
        daq_state_feed(&state, &raw, 100);
    }
    CHECK(state.nodes[2].sequence_gaps == 2);
    daq_raw_frame_t first = diagnostics_frame(2, 0);
    daq_state_feed(&state, &first, 200);
    daq_raw_frame_t rebooted = diagnostics_frame(2, 1);  /* resets changed: the node rebooted */
    daq_state_feed(&state, &rebooted, 300);
    daq_raw_frame_t restart = event_frame(2, 0);
    daq_state_feed(&state, &restart, 400);
    CHECK(state.nodes[2].sequence_gaps == 2);
    daq_raw_frame_t wrapped = event_frame(2, 63);         /* 0 -> 63 wraps: 62 missed */
    daq_state_feed(&state, &wrapped, 500);
    CHECK(state.nodes[2].sequence_gaps == 64);
}

static void test_state_event_history(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    for (int i = 0; i < 20; i++) {
        daq_raw_frame_t raw = event_frame(5, (uint8_t)i);
        daq_state_feed(&state, &raw, (uint64_t)i);
    }
    const daq_node_state_t *node = &state.nodes[5];
    CHECK(node->event_total == 20);
    unsigned newest = (node->event_next + DAQ_EVENT_HISTORY - 1) % DAQ_EVENT_HISTORY;
    CHECK(node->events[newest].u.event.sequence == 19 && node->event_ms[newest] == 19);
    CHECK(node->sequence_gaps == 0);
}

static void test_state_master_and_counters(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    uint8_t bytes[8] = {0};
    bytes[7] = 0x80;                                  /* recording bit */
    bytes[0] = 0x40;
    daq_raw_frame_t beacon = frame(CAN_ID_MASTER_TIME, bytes, 8);
    daq_state_feed(&state, &beacon, 50);
    CHECK(state.master.beacon_seen && state.master.recording && state.master.beacon_ms == 50);
    CHECK(state.master.master_us == 0x40);
    daq_raw_frame_t start = frame(CAN_ID_START, NULL, 0);
    daq_state_feed(&state, &start, 60);
    CHECK(state.master.last_command == DAQ_FRAME_START && state.master.command_ms == 60);
    daq_raw_frame_t other = frame(0x123, bytes, 8);
    daq_state_feed(&state, &other, 70);
    daq_raw_frame_t bad = frame(CAN_ID_FRONT_BRAKE, bytes, 3);
    daq_state_feed(&state, &bad, 80);
    CHECK(state.unknown_frames == 1 && state.malformed_frames == 1 && state.frames == 4);
    daq_raw_frame_t data = frame(CAN_ID_UPDATE_DATA, bytes, 8);
    daq_state_feed(&state, &data, 90);
    CHECK(state.update_frames == 1);
    daq_raw_frame_t aim = frame(CAN_ID_AIM_WIRE, (const uint8_t[]){0xD0, 0x07, 1, 0}, 4);
    daq_state_feed(&state, &aim, 100);
    int aim_index = daq_signal_index(CAN_ID_AIM_WIRE);
    CHECK(state.signals[aim_index].value == 2000 && daq_signal_fresh(&state, aim_index, 100));
}

static void test_state_node_reports(void)
{
    static daq_state_t state;
    daq_state_init(&state);
    daq_raw_frame_t report = frame(CAN_ID_NODE_STATE_BASE + 7, (const uint8_t[]){1, 3, 0}, 3);
    daq_state_feed(&state, &report, 10);
    CHECK(state.nodes[7].state_seen && state.nodes[7].state == 1 && state.nodes[7].reason == 3);
    node_identity_frame_t identity = {.elf_sha256 = {9, 8, 7, 6, 5}, .protocol_hash = 0x1234};
    daq_raw_frame_t id_frame = frame(CAN_ID_NODE_IDENTITY_BASE + 7, (const uint8_t *)&identity, 8);
    daq_state_feed(&state, &id_frame, 20);
    CHECK(state.nodes[7].identity_seen && state.nodes[7].protocol_hash == 0x1234);
    daq_raw_frame_t diag = frame(CAN_ID_NODE_DIAGNOSTICS_BASE + 7, (const uint8_t[]){3, 4, 5, 6}, 4);
    daq_state_feed(&state, &diag, 30);
    CHECK(state.nodes[7].tx_errors == 3 && state.nodes[7].rx_errors == 4 && state.nodes[7].resets == 5);
    CHECK(state.nodes[7].last_ms == 30);
}

int main(void)
{
    test_decode_brake_sample();
    test_decode_negative_sample();
    test_decode_beacon();
    test_decode_start_stop();
    test_decode_node_state();
    test_decode_status_matches_struct();
    test_decode_event_matches_struct();
    test_decode_diagnostics();
    test_decode_identity();
    test_decode_update_frames();
    test_decode_aim();
    test_decode_malformed_and_unknown();
    test_signal_table();
    test_state_fresh_then_stale();
    test_state_online_then_offline();
    test_state_rate();
    test_state_event_gaps();
    test_state_event_history();
    test_state_master_and_counters();
    test_state_node_reports();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
