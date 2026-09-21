#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int64_t timestamp_us;
    int32_t engine_rpm;
    bool spark;
} engine_event_t;
bool engine_rpm_next_event(engine_event_t *event);
uint32_t engine_rpm_dropped_events(void);
void engine_rpm_discard_pending(void);
