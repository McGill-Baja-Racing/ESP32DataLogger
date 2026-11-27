#ifndef DAQ_STATE_H
#define DAQ_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "can/daq_decode.h"

/* The live picture of the DAQ bus: latest value per signal, what each node last
 * reported, and the master's beacon. Pure: frames come in with their arrival
 * time on the Pi's monotonic clock (ms); nothing here reads a clock or a socket.
 * Freshness, rates and "last seen" use only that arrival time, never the nodes'
 * timestamps. */

#define DAQ_STALE_MS 500           /* a signal with no frame for this long shows as unavailable */
#define DAQ_NODE_OFFLINE_MS 3000   /* the master's rule: no frame of any kind for 3 s */
#define DAQ_EVENT_HISTORY 16

typedef struct {
    bool seen;
    int32_t value;
    uint32_t node_timestamp_ms;   /* as sent by the node; for display only */
    uint64_t last_ms;             /* arrival on the Pi */
    uint32_t count, count_at_rate;
    float rate_hz;
} daq_signal_state_t;

typedef struct {
    bool seen;
    uint64_t last_ms;                       /* any frame from this node */
    bool state_seen;
    uint8_t state, reason, reset_reason;    /* last state report */
    bool status_seen;
    uint32_t faults;
    uint8_t health, mode, sensors[NODE_SENSOR_SLOTS];
    bool diagnostics_seen;
    uint8_t tx_errors, rx_errors, resets, reset_reason_diag;
    bool identity_seen;
    uint8_t elf_sha256[NODE_IDENTITY_HASH_LENGTH];
    uint16_t protocol_hash;
    bool pending;
    daq_frame_t events[DAQ_EVENT_HISTORY];  /* ring; the newest is before event_next */
    uint64_t event_ms[DAQ_EVENT_HISTORY];
    unsigned event_next, event_total;
    bool sequence_seen;
    uint8_t last_sequence;
    /* Events missing from the sequence. The Pi cannot tell a frame it dropped
     * from one the node lost, so show this next to the Pi's own losses. */
    uint32_t sequence_gaps;
} daq_node_state_t;

typedef struct {
    bool beacon_seen, recording;
    uint64_t master_us;     /* master uptime from the last beacon */
    uint64_t beacon_ms;     /* arrival of the last beacon */
    daq_frame_kind_t last_command;  /* DAQ_FRAME_START or DAQ_FRAME_STOP; UNKNOWN if none seen */
    uint64_t command_ms;
} daq_master_state_t;

typedef struct {
    daq_master_state_t master;
    daq_signal_state_t signals[DAQ_MAX_SIGNALS];   /* indexed like daq_signals() */
    daq_node_state_t nodes[CAN_NODE_SLOT_COUNT];   /* 1..8 used */
    uint32_t frames, unknown_frames, malformed_frames, update_frames;
    uint64_t rate_ms;                              /* when rates were last computed */
} daq_state_t;

void daq_state_init(daq_state_t *state);

/* Takes one received frame, which arrived at now_ms. */
void daq_state_feed(daq_state_t *state, const daq_raw_frame_t *raw, uint64_t now_ms);

/* Recomputes every signal's rate once 1000 ms or more passed since the last time. */
void daq_state_update_rates(daq_state_t *state, uint64_t now_ms);

/* True if the signal at this daq_signals() index arrived within DAQ_STALE_MS. */
bool daq_signal_fresh(const daq_state_t *state, int index, uint64_t now_ms);

/* True if node 1..8 sent any frame within DAQ_NODE_OFFLINE_MS. */
bool daq_node_online(const daq_state_t *state, uint8_t node, uint64_t now_ms);

#endif /* DAQ_STATE_H */
