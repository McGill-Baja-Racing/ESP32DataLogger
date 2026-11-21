#include "aim/aim_adapter.h"
#include "protocol/app_protocol.h"

bool aim_decode(aim_tracker_t *tracker, const can_message_t *frame, can_message_t *sample)
{
    if (frame->id != AIM_CAN_ID) return false;
    if (frame->extended || frame->remote || frame->dlc != 4) {
        tracker->malformed_frames++;
        return false;
    }
    /* Wire bytes 0–1 are RPM; bytes 2–3 are the wrapping frame counter. */
    uint16_t counter = (uint16_t)(frame->data >> 16);
    /* Wrap is expected. Jumps may be loss or a restart, so only count discontinuities. */
    if (tracker->seen && counter != (uint16_t)(tracker->last_counter + 1))
        tracker->counter_discontinuities++;
    tracker->last_counter = counter;
    tracker->seen = true;
    tracker->valid_frames++;
    tracker->last_received_us = frame->received_us;

    /* Normalize for the existing logger: low 32 bits RPM, high 32 bits master ms.
     * Reception time is not the AiM acquisition time; no latency correction is assumed. */
    *sample = (can_message_t){
        .id = CAN_ID_AIM_RPM, .dlc = 8,
        .data = ((uint64_t)(uint32_t)(frame->received_us / 1000) << 32) |
                (uint16_t)frame->data,
        .received_us = frame->received_us,
    };
    return true;
}

aim_status_t aim_tracker_status(const aim_tracker_t *tracker, int64_t now_us)
{
    int64_t age = tracker->seen ? now_us - tracker->last_received_us : -1;
    return (aim_status_t){
        .state = !tracker->seen ? "waiting" :
                 age >= AIM_STALE_US ? "stale" : "receiving",
        .last_frame_age_ms = age < 0 ? -1 : age / 1000,
        .valid_frames = tracker->valid_frames,
        .malformed_frames = tracker->malformed_frames,
        .counter_discontinuities = tracker->counter_discontinuities,
    };
}
