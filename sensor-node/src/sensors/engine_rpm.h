#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int64_t timestamp_us;
    int32_t engine_rpm;
    bool spark;
} engine_event_t;

/**
 * @brief Wait up to one tick for the next engine event.
 *
 * A spark gives its RPM, or 0 for the first spark or an RPM outside 1000-6000.
 * With no spark for 100 ms it gives a non-spark event with RPM 0, repeated
 * every 100 ms.
 *
 * @param event Filled in when this returns true.
 * @return true if an event was returned.
 */
bool engine_rpm_next_event(engine_event_t *event);

/**
 * @brief Read and reset the count of sparks lost to a full queue.
 *
 * @return Sparks lost since the last call.
 */
uint32_t engine_rpm_dropped_events(void);

/**
 * @brief Clear queued sparks and restart RPM measurement.
 *
 * The next spark gives RPM 0. Also resets the dropped count.
 */
void engine_rpm_discard_pending(void);
