#ifndef DAQ_DECODE_H
#define DAQ_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol/app_protocol.h"

/* Decodes one DAQ CAN frame into a typed record, following the shared protocol
 * header and docs/PROTOCOL.md. Pure functions: no I/O, no clock. Bit fields are
 * read with shifts, never by casting to the header's packed structs. */

/* Every sample ID, then AiM 0x500. */
#define DAQ_MAX_SIGNALS 17

typedef struct {
    uint32_t id;        /* 11-bit, or 29-bit if extended */
    bool extended, remote;
    uint8_t len;        /* 0..8 */
    uint8_t data[8];
} daq_raw_frame_t;

typedef enum {
    DAQ_FRAME_UNKNOWN,
    DAQ_FRAME_MALFORMED,  /* a DAQ ID with the wrong length */
    DAQ_FRAME_STOP,
    DAQ_FRAME_START,
    DAQ_FRAME_BEACON,
    DAQ_FRAME_NODE_STATE,
    DAQ_FRAME_SAMPLE,
    DAQ_FRAME_STATUS,
    DAQ_FRAME_EVENT,
    DAQ_FRAME_DIAGNOSTICS,
    DAQ_FRAME_IDENTITY,
    DAQ_FRAME_UPDATE_COMMAND,
    DAQ_FRAME_UPDATE_DATA,
    DAQ_FRAME_UPDATE_REPLY,
    DAQ_FRAME_AIM,
    DAQ_FRAME_KIND_COUNT,
} daq_frame_kind_t;

typedef struct {
    daq_frame_kind_t kind;
    uint8_t node;   /* sender 1..8 for node frames (0 for the master's own health),
                     * target for update commands (0xFF for all), else 0 */
    union {
        struct { bool recording; uint64_t master_us; } beacon;
        struct { uint8_t state, reason, reset_reason; } node_state;
        struct { uint32_t id; int32_t value; uint32_t timestamp_ms; } sample;
        struct { uint32_t faults; uint8_t health, mode; uint8_t sensors[NODE_SENSOR_SLOTS]; } status;
        struct { uint8_t code, error, subject, sequence; uint16_t count; uint32_t timestamp_ms; } event;
        struct { uint8_t tx_errors, rx_errors, resets, reset_reason; } diagnostics;
        struct { uint8_t elf_sha256[NODE_IDENTITY_HASH_LENGTH]; uint16_t protocol_hash; bool pending; } identity;
        struct { uint8_t op; } update_command;
        struct { uint8_t reply, error; } update_reply;
        struct { uint16_t rpm, counter; } aim;
    } u;
} daq_frame_t;

typedef struct {
    uint32_t id;
    const char *name;
    const char *unit;
    uint8_t node;   /* 0 for AiM */
} daq_signal_t;

/* Decodes one frame. A DAQ ID with the wrong length gives DAQ_FRAME_MALFORMED;
 * extended, remote and unrelated frames give DAQ_FRAME_UNKNOWN. */
daq_frame_t daq_decode(const daq_raw_frame_t *raw);

/* The table of signals shown with a value: every sample ID, then AiM RPM. */
const daq_signal_t *daq_signals(size_t *count);

/* Index of a standard ID in daq_signals(), or -1. */
int daq_signal_index(uint32_t id);

/* Short name of a frame kind, such as "status". */
const char *daq_frame_kind_name(daq_frame_kind_t kind);

#endif /* DAQ_DECODE_H */
