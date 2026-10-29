#include "time/clock_discipline.h"

#include <stdlib.h>

int64_t clock_discipline_utc_us(const clock_discipline_t *clock, int64_t uptime_us)
{
    if (!clock->synchronized) return 0;
    int64_t elapsed_us = uptime_us - clock->anchor_uptime_us;

    /* Add the correction in proportion to elapsed time, up to all of it once
     * the correction period has passed. */
    int64_t correcting_us = elapsed_us < CLOCK_DISCIPLINE_CORRECTION_US
                          ? elapsed_us : CLOCK_DISCIPLINE_CORRECTION_US;
    int64_t correction_so_far_us =
        clock->correction_us * correcting_us / CLOCK_DISCIPLINE_CORRECTION_US;

    return clock->anchor_utc_us + elapsed_us + correction_so_far_us;
}

/* Start the line exactly at the GPS time, discarding any previous estimate. */
static void jump_to(clock_discipline_t *clock, int64_t uptime_us, int64_t utc_us)
{
    *clock = (clock_discipline_t){
        .synchronized = true,
        .anchor_uptime_us = uptime_us,
        .anchor_utc_us = utc_us,
    };
}

clock_observation_t clock_discipline_observe(clock_discipline_t *clock,
                                             int64_t uptime_us, int64_t utc_us,
                                             int64_t *error_us)
{
    *error_us = 0;
    if (!clock->synchronized) {
        jump_to(clock, uptime_us, utc_us);
        return CLOCK_OBSERVATION_ANCHORED;
    }

    int64_t estimate_us = clock_discipline_utc_us(clock, uptime_us);
    *error_us = utc_us - estimate_us;

    if (llabs(*error_us) > CLOCK_DISCIPLINE_MAX_ERROR_US) {
        clock->rejected_in_a_row++;
        if (clock->rejected_in_a_row < CLOCK_DISCIPLINE_STEP_AFTER_REJECTIONS) {
            return CLOCK_OBSERVATION_REJECTED;
        }
        jump_to(clock, uptime_us, utc_us);
        return CLOCK_OBSERVATION_STEPPED;
    }

    /* Restart the line from where the estimate is now, so time does not jump,
     * and correct the new error gradually from here. */
    clock->anchor_uptime_us = uptime_us;
    clock->anchor_utc_us = estimate_us;
    clock->correction_us = *error_us;
    clock->rejected_in_a_row = 0;
    return CLOCK_OBSERVATION_CORRECTED;
}
