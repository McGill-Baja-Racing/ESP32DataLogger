#pragma once

#include "can/can_master.h"
#include "protocol/app_protocol.h"

/* Owned by the CAN dispatch task. Missing/stale wheel data produces no pair.
 * Unsigned timestamp subtraction handles the 32-bit millisecond rollover. */
typedef struct {
    can_message_t wheel;
    int64_t received_us;
    bool valid;
} rpm_pairing_t;

static inline bool rpm_pairing_update(rpm_pairing_t *pairing,
                                      const can_message_t *message,
                                      int64_t now_us, can_message_t *result)
{
    if (message->dlc != 8) return false;
    if (message->id == CAN_ID_BEARING_ENCODER) {
        pairing->wheel = *message;
        pairing->received_us = now_us;
        pairing->valid = true;
        return false;
    }
    if (message->id != CAN_ID_ENGINE_RPM || !pairing->valid) return false;
    uint32_t engine_ms = (uint32_t)(message->data >> 32);
    uint32_t wheel_ms = (uint32_t)(pairing->wheel.data >> 32);
    if (now_us < pairing->received_us ||
        now_us - pairing->received_us > 100000 ||
        (uint32_t)(engine_ms - wheel_ms) > 100) return false;
    *result = (can_message_t){
        .id = CAN_ID_ENGINE_WHEEL_RPM,
        .dlc = 8,
        .data = ((uint64_t)engine_ms << 32) |
                (pairing->wheel.data & UINT32_MAX),
    };
    return true;
}
