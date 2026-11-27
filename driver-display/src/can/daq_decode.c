#include "can/daq_decode.h"

#include <string.h>

static const daq_signal_t signals[DAQ_MAX_SIGNALS] = {
    {CAN_ID_FRONT_BRAKE, "front brake", "psi", 1},
    {CAN_ID_REAR_BRAKE, "rear brake", "psi", 1},
    {CAN_ID_MPU_ACCEL_X, "accel X", "mg", 3},
    {CAN_ID_MPU_ACCEL_Y, "accel Y", "mg", 3},
    {CAN_ID_MPU_ACCEL_Z, "accel Z", "mg", 3},
    {CAN_ID_MPU_GYRO_X, "gyro X", "mdeg/s", 3},
    {CAN_ID_MPU_GYRO_Y, "gyro Y", "mdeg/s", 3},
    {CAN_ID_MPU_GYRO_Z, "gyro Z", "mdeg/s", 3},
    {CAN_ID_BEARING_ENCODER, "bearing RPM", "rpm", 4},
    {CAN_ID_GENERIC_ADC, "generic ADC", "mV", 6},
    {CAN_ID_ENGINE_RPM, "engine RPM", "rpm", 5},
    {CAN_ID_ENGINE_SPARK, "spark", "", 5},
    {CAN_ID_FRONT_LIN_1, "front pot 1", "mm", 7},
    {CAN_ID_FRONT_LIN_2, "front pot 2", "mm", 7},
    {CAN_ID_REAR_LIN_1, "rear pot 1", "mm", 8},
    {CAN_ID_REAR_LIN_2, "rear pot 2", "mm", 8},
    {CAN_ID_AIM_WIRE, "AiM RPM", "rpm", 0},
};

static const char *const kind_names[DAQ_FRAME_KIND_COUNT] = {
    [DAQ_FRAME_UNKNOWN] = "unknown",
    [DAQ_FRAME_MALFORMED] = "malformed",
    [DAQ_FRAME_STOP] = "stop",
    [DAQ_FRAME_START] = "start",
    [DAQ_FRAME_BEACON] = "beacon",
    [DAQ_FRAME_NODE_STATE] = "state",
    [DAQ_FRAME_SAMPLE] = "sample",
    [DAQ_FRAME_STATUS] = "status",
    [DAQ_FRAME_EVENT] = "event",
    [DAQ_FRAME_DIAGNOSTICS] = "diagnostics",
    [DAQ_FRAME_IDENTITY] = "identity",
    [DAQ_FRAME_UPDATE_COMMAND] = "update command",
    [DAQ_FRAME_UPDATE_DATA] = "update data",
    [DAQ_FRAME_UPDATE_REPLY] = "update reply",
    [DAQ_FRAME_AIM] = "aim",
};

const daq_signal_t *daq_signals(size_t *count)
{
    if (count) *count = DAQ_MAX_SIGNALS;
    return signals;
}

int daq_signal_index(uint32_t id)
{
    for (int i = 0; i < DAQ_MAX_SIGNALS; i++) {
        if (signals[i].id == id) return i;
    }
    return -1;
}

const char *daq_frame_kind_name(daq_frame_kind_t kind)
{
    return kind < DAQ_FRAME_KIND_COUNT ? kind_names[kind] : "?";
}

/* Little-endian reads: the bus carries every multi-byte field little-endian. */
static uint32_t u32_at(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static uint64_t u64_at(const uint8_t *bytes)
{
    return (uint64_t)u32_at(bytes) | (uint64_t)u32_at(bytes + 4) << 32;
}

/* Node N of a frame family at base + N; 0 (the master) only when allowed. */
static bool node_of(uint32_t id, uint32_t base, bool master_allowed, uint8_t *node)
{
    if (id < base || id >= base + CAN_NODE_SLOT_COUNT) return false;
    uint32_t n = id - base;
    if (n == 0 && !master_allowed) return false;
    *node = (uint8_t)n;
    return true;
}

static daq_frame_t result(daq_frame_kind_t kind, uint8_t node)
{
    daq_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.kind = kind;
    frame.node = node;
    return frame;
}

/* Status payload: faults bits 0-16, health 17-18, mode 19, sensors 20-31 (2 bits per slot). */
static daq_frame_t decode_status(const daq_raw_frame_t *raw, uint8_t node)
{
    daq_frame_t frame = result(DAQ_FRAME_STATUS, node);
    uint32_t word = u32_at(raw->data);
    frame.u.status.faults = word & ((1u << HEALTH_STATUS_FAULT_BITS) - 1);
    frame.u.status.health = (uint8_t)(word >> HEALTH_STATUS_FAULT_BITS & 0x3);
    frame.u.status.mode = (uint8_t)(word >> (HEALTH_STATUS_FAULT_BITS + 2) & 0x1);
    uint32_t sensors = word >> (HEALTH_STATUS_FAULT_BITS + 3);
    for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS; slot++) {
        frame.u.status.sensors[slot] = (uint8_t)(sensors >> (2 * slot) & 0x3);
    }
    return frame;
}

/* Event payload: code bits 0-4, error 5-8, subject 9-16, sequence 17-22, count 23-31. */
static daq_frame_t decode_event(const daq_raw_frame_t *raw, uint8_t node)
{
    daq_frame_t frame = result(DAQ_FRAME_EVENT, node);
    uint32_t word = u32_at(raw->data);
    frame.u.event.code = (uint8_t)(word & 0x1F);
    frame.u.event.error = (uint8_t)(word >> 5 & 0xF);
    frame.u.event.subject = (uint8_t)(word >> 9 & 0xFF);
    frame.u.event.sequence = (uint8_t)(word >> 17 & HEALTH_EVENT_SEQUENCE_MASK);
    frame.u.event.count = (uint16_t)(word >> 23 & HEALTH_EVENT_COUNT_MAX);
    frame.u.event.timestamp_ms = u32_at(raw->data + 4);
    return frame;
}

daq_frame_t daq_decode(const daq_raw_frame_t *raw)
{
    const uint32_t id = raw->id;
    const uint8_t len = raw->len;
    const uint8_t *d = raw->data;
    uint8_t node = 0;

    if (raw->extended || raw->remote || len > 8) return result(DAQ_FRAME_UNKNOWN, 0);

    if (id == CAN_ID_STOP) return result(DAQ_FRAME_STOP, 0);
    if (id == CAN_ID_START) return result(DAQ_FRAME_START, 0);
    if (id == CAN_ID_MASTER_TIME) {
        if (len != 8) return result(DAQ_FRAME_MALFORMED, 0);
        daq_frame_t frame = result(DAQ_FRAME_BEACON, 0);
        uint64_t value = u64_at(d);
        frame.u.beacon.recording = (value & CAN_MASTER_TIME_RECORDING_FLAG) != 0;
        frame.u.beacon.master_us = value & CAN_MASTER_TIME_VALUE_MASK;
        return frame;
    }
    if (protocol_is_sensor_id(id)) {
        node = protocol_sensor_node_id(id);
        if (len != 8) return result(DAQ_FRAME_MALFORMED, node);
        daq_frame_t frame = result(DAQ_FRAME_SAMPLE, node);
        frame.u.sample.id = id;
        frame.u.sample.value = (int32_t)u32_at(d);
        frame.u.sample.timestamp_ms = u32_at(d + 4);
        return frame;
    }
    if (node_of(id, CAN_ID_NODE_STATE_BASE, false, &node)) {
        if (len < CAN_NODE_STATE_LENGTH) return result(DAQ_FRAME_MALFORMED, node);
        daq_frame_t frame = result(DAQ_FRAME_NODE_STATE, node);
        frame.u.node_state.state = d[0];
        frame.u.node_state.reason = d[1];
        frame.u.node_state.reset_reason = d[2];
        return frame;
    }
    if (node_of(id, CAN_ID_NODE_STATUS_BASE, true, &node)) {
        if (len < NODE_HEALTH_PAYLOAD) return result(DAQ_FRAME_MALFORMED, node);
        return decode_status(raw, node);
    }
    if (node_of(id, CAN_ID_NODE_EVENT_BASE, true, &node)) {
        if (len != 8) return result(DAQ_FRAME_MALFORMED, node);
        return decode_event(raw, node);
    }
    if (node_of(id, CAN_ID_NODE_DIAGNOSTICS_BASE, true, &node)) {
        if (len < NODE_HEALTH_PAYLOAD) return result(DAQ_FRAME_MALFORMED, node);
        daq_frame_t frame = result(DAQ_FRAME_DIAGNOSTICS, node);
        frame.u.diagnostics.tx_errors = d[0];
        frame.u.diagnostics.rx_errors = d[1];
        frame.u.diagnostics.resets = d[2];
        frame.u.diagnostics.reset_reason = d[3];
        return frame;
    }
    if (node_of(id, CAN_ID_NODE_IDENTITY_BASE, false, &node)) {
        if (len != 8) return result(DAQ_FRAME_MALFORMED, node);
        daq_frame_t frame = result(DAQ_FRAME_IDENTITY, node);
        memcpy(frame.u.identity.elf_sha256, d, NODE_IDENTITY_HASH_LENGTH);
        frame.u.identity.protocol_hash = (uint16_t)(d[5] | d[6] << 8);
        frame.u.identity.pending = (d[7] & NODE_IDENTITY_PENDING) != 0;
        return frame;
    }
    if (node_of(id, CAN_ID_NODE_UPDATE_BASE, false, &node)) {
        if (len < 1) return result(DAQ_FRAME_MALFORMED, node);
        daq_frame_t frame = result(DAQ_FRAME_UPDATE_REPLY, node);
        frame.u.update_reply.reply = d[0];
        frame.u.update_reply.error = (d[0] == UPDATE_REPLY_ERROR && len >= 2) ? d[1] : 0;
        return frame;
    }
    if (id == CAN_ID_UPDATE_COMMAND) {
        if (len < 2) return result(DAQ_FRAME_MALFORMED, 0);
        daq_frame_t frame = result(DAQ_FRAME_UPDATE_COMMAND, d[0]);
        frame.u.update_command.op = d[1];
        return frame;
    }
    if (id == CAN_ID_UPDATE_DATA) {
        return len >= 1 ? result(DAQ_FRAME_UPDATE_DATA, 0) : result(DAQ_FRAME_MALFORMED, 0);
    }
    if (id == CAN_ID_AIM_WIRE) {
        if (len != 4) return result(DAQ_FRAME_MALFORMED, 0);
        daq_frame_t frame = result(DAQ_FRAME_AIM, 0);
        frame.u.aim.rpm = (uint16_t)(d[0] | d[1] << 8);
        frame.u.aim.counter = (uint16_t)(d[2] | d[3] << 8);
        return frame;
    }
    return result(DAQ_FRAME_UNKNOWN, 0);
}
