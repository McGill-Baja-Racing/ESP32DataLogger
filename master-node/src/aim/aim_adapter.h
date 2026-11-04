#pragma once

#include "can/can_master.h"

/* Incoming wire ID; decoded samples use the master-only CAN_ID_AIM_RPM. */
#define AIM_CAN_ID 0x500

/* Nominal display rate only; reception is not limited to this frequency. */
#ifndef AIM_OUTPUT_RATE_HZ
#define AIM_OUTPUT_RATE_HZ 20
#endif
#if AIM_OUTPUT_RATE_HZ != 20 && AIM_OUTPUT_RATE_HZ != 100
#error "AIM_OUTPUT_RATE_HZ must be 20 or 100"
#endif
#define AIM_STALE_US 500000LL

typedef struct {
    uint32_t valid_frames, malformed_frames, counter_discontinuities;
    bool seen;
    int64_t last_received_us;
    uint16_t last_counter;
} aim_tracker_t;

typedef struct {
    const char *state;
    int64_t last_frame_age_ms; /* -1 before the first valid frame */
    uint32_t valid_frames, malformed_frames, counter_discontinuities;
} aim_status_t;

/* Decode RPM/counter, update diagnostics, and produce a value/timestamp sample.
 * Returns false for unrelated or malformed frames; sample is then untouched. */
bool aim_decode(aim_tracker_t *tracker, const can_message_t *frame, can_message_t *sample);
/* Derive connection state from the last valid reception, independently of logging. */
aim_status_t aim_tracker_status(const aim_tracker_t *tracker, int64_t now_us, bool available);

/* Thread-safe decoder wrapper; diagnostics update even outside a recording. */
bool aim_adapter_receive(const can_message_t *frame, can_message_t *sample);
/* Snapshot diagnostics for the web/serial status readers. */
void aim_adapter_status(aim_status_t *status);
