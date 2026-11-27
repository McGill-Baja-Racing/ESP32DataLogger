#include "can/daq_state.h"

#include <string.h>

void daq_state_init(daq_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->master.last_command = DAQ_FRAME_UNKNOWN;
}

static void touch_node(daq_state_t *state, uint8_t node, uint64_t now_ms)
{
    if (node == 0 || node >= CAN_NODE_SLOT_COUNT) return;
    state->nodes[node].seen = true;
    state->nodes[node].last_ms = now_ms;
}

static void record_signal(daq_state_t *state, uint32_t id, int32_t value, uint32_t node_timestamp_ms,
                          uint64_t now_ms)
{
    int index = daq_signal_index(id);
    if (index < 0) return;
    daq_signal_state_t *signal = &state->signals[index];
    signal->seen = true;
    signal->value = value;
    signal->node_timestamp_ms = node_timestamp_ms;
    signal->last_ms = now_ms;
    signal->count++;
}

/* Same rule as the master's check_sequence(): a boot event restarts the check,
 * a repeated sequence is a copy CAN delivered twice, and skipped numbers are gaps. */
static void record_event(daq_node_state_t *node, const daq_frame_t *event, uint64_t now_ms)
{
    uint8_t sequence = event->u.event.sequence;
    if (node->sequence_seen && event->u.event.code != HEALTH_BOOT) {
        if (sequence == node->last_sequence) return;
        node->sequence_gaps += (uint8_t)(sequence - node->last_sequence - 1) & HEALTH_EVENT_SEQUENCE_MASK;
    }
    node->sequence_seen = true;
    node->last_sequence = sequence;
    node->events[node->event_next] = *event;
    node->event_ms[node->event_next] = now_ms;
    node->event_next = (node->event_next + 1) % DAQ_EVENT_HISTORY;
    node->event_total++;
}

void daq_state_feed(daq_state_t *state, const daq_raw_frame_t *raw, uint64_t now_ms)
{
    daq_frame_t frame = daq_decode(raw);
    daq_node_state_t *node = frame.node < CAN_NODE_SLOT_COUNT ? &state->nodes[frame.node] : NULL;
    state->frames++;

    switch (frame.kind) {
    case DAQ_FRAME_UNKNOWN:
        state->unknown_frames++;
        return;
    case DAQ_FRAME_MALFORMED:
        state->malformed_frames++;
        return;
    case DAQ_FRAME_START:
    case DAQ_FRAME_STOP:
        state->master.last_command = frame.kind;
        state->master.command_ms = now_ms;
        return;
    case DAQ_FRAME_BEACON:
        state->master.beacon_seen = true;
        state->master.recording = frame.u.beacon.recording;
        state->master.master_us = frame.u.beacon.master_us;
        state->master.beacon_ms = now_ms;
        return;
    case DAQ_FRAME_SAMPLE:
        record_signal(state, frame.u.sample.id, frame.u.sample.value, frame.u.sample.timestamp_ms, now_ms);
        touch_node(state, frame.node, now_ms);
        return;
    case DAQ_FRAME_AIM:
        record_signal(state, CAN_ID_AIM_WIRE, frame.u.aim.rpm, 0, now_ms);
        return;
    case DAQ_FRAME_UPDATE_COMMAND:
    case DAQ_FRAME_UPDATE_DATA:
        state->update_frames++;
        return;
    default:
        break;
    }

    /* Frames from a node: state, health, identity, update replies. The master's
     * own health (node 0) never travels on the bus, so it is ignored here. */
    if (frame.node == 0 || !node) return;
    touch_node(state, frame.node, now_ms);
    switch (frame.kind) {
    case DAQ_FRAME_NODE_STATE:
        node->state_seen = true;
        node->state = frame.u.node_state.state;
        node->reason = frame.u.node_state.reason;
        node->reset_reason = frame.u.node_state.reset_reason;
        break;
    case DAQ_FRAME_STATUS:
        node->status_seen = true;
        node->faults = frame.u.status.faults;
        node->health = frame.u.status.health;
        node->mode = frame.u.status.mode;
        memcpy(node->sensors, frame.u.status.sensors, sizeof(node->sensors));
        break;
    case DAQ_FRAME_EVENT:
        record_event(node, &frame, now_ms);
        break;
    case DAQ_FRAME_DIAGNOSTICS:
        /* A node that reset restarts its event sequence at 0: not a gap. */
        if (node->diagnostics_seen && frame.u.diagnostics.resets != node->resets) {
            node->sequence_seen = false;
        }
        node->diagnostics_seen = true;
        node->tx_errors = frame.u.diagnostics.tx_errors;
        node->rx_errors = frame.u.diagnostics.rx_errors;
        node->resets = frame.u.diagnostics.resets;
        node->reset_reason_diag = frame.u.diagnostics.reset_reason;
        break;
    case DAQ_FRAME_IDENTITY:
        node->identity_seen = true;
        memcpy(node->elf_sha256, frame.u.identity.elf_sha256, sizeof(node->elf_sha256));
        node->protocol_hash = frame.u.identity.protocol_hash;
        node->pending = frame.u.identity.pending;
        break;
    case DAQ_FRAME_UPDATE_REPLY:
        state->update_frames++;
        break;
    default:
        break;
    }
}

void daq_state_update_rates(daq_state_t *state, uint64_t now_ms)
{
    if (now_ms < state->rate_ms + 1000) return;
    float seconds = (float)(now_ms - state->rate_ms) / 1000.0f;
    for (int i = 0; i < DAQ_MAX_SIGNALS; i++) {
        daq_signal_state_t *signal = &state->signals[i];
        signal->rate_hz = (float)(signal->count - signal->count_at_rate) / seconds;
        signal->count_at_rate = signal->count;
    }
    state->rate_ms = now_ms;
}

bool daq_signal_fresh(const daq_state_t *state, int index, uint64_t now_ms)
{
    if (index < 0 || index >= DAQ_MAX_SIGNALS) return false;
    const daq_signal_state_t *signal = &state->signals[index];
    return signal->seen && now_ms - signal->last_ms < DAQ_STALE_MS;
}

bool daq_node_online(const daq_state_t *state, uint8_t node, uint64_t now_ms)
{
    if (node == 0 || node >= CAN_NODE_SLOT_COUNT) return false;
    return state->nodes[node].seen && now_ms - state->nodes[node].last_ms < DAQ_NODE_OFFLINE_MS;
}
