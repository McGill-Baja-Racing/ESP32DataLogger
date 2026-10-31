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
 * A spark gives spark = true with the RPM from the time since the previous
 * spark. That RPM is 0 for the first spark or an interval outside
 * 1000-6000 RPM. With no spark for 100 ms, gives a spark = false event with
 * RPM 0 so the log shows the engine stopped; this repeats every 100 ms.
 *
 * @param event Filled in when this returns true.
 *
 * @return true if event was filled in, false if nothing happened.
 */
bool engine_rpm_next_event(engine_event_t *event);

/**
 * @brief Read and reset the number of sparks lost to a full event queue.
 *
 * @return Sparks lost since the last call.
 */
uint32_t engine_rpm_dropped_events(void);

/**
 * @brief Clear queued sparks and restart RPM measurement.
 *
 * The next spark counts as the first, so it gives RPM 0. Also resets the
 * dropped count. Called on start and on CAN bus-off.
 */
void engine_rpm_discard_pending(void);
