/* Host tests for the DAQ decoder and live table. Run with `make test`. */
#include <stdio.h>
#include <string.h>

#include "can/daq_decode.h"

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
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
