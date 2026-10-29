#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Keeps a UTC estimate locked to GPS without ever making time jump.
 *
 * The estimate is a straight line through an anchor point:
 *
 *     utc(uptime) = anchor_utc + (uptime - anchor_uptime) + correction
 *
 * Each GPS observation is compared with the line. A small error is not applied
 * at once; it is added gradually over CLOCK_DISCIPLINE_CORRECTION_US, so
 * timestamps stay in order and one noisy GPS sentence barely moves the clock.
 * Once the correction is complete, time runs at the ESP32 rate again.
 *
 * Plain C with no ESP-IDF calls, so the host tests can run it directly. */

/* Time over which each error is gradually corrected. */
#define CLOCK_DISCIPLINE_CORRECTION_US          INT64_C(32000000)
/* Errors larger than this are treated as a bad GPS sentence, not drift. */
#define CLOCK_DISCIPLINE_MAX_ERROR_US           INT64_C(200000)
/* Once every observation has been rejected for this long, trust GPS and jump
 * to its time. A duration, so it does not depend on the GPS update rate (5 Hz
 * normally, 1 Hz if the receiver was not configured)... */
#define CLOCK_DISCIPLINE_STEP_AFTER_US          INT64_C(5000000)
/* ...and at least this many rejections, so two outliers either side of a GPS
 * dropout cannot step the clock. */
#define CLOCK_DISCIPLINE_STEP_AFTER_REJECTIONS  5

typedef struct {
    bool synchronized;           /* False until the first GPS observation. */
    int64_t anchor_uptime_us;    /* ESP32 uptime at the last accepted observation. */
    int64_t anchor_utc_us;       /* UTC estimate at that same moment. */
    int64_t correction_us;       /* Error still being added over the correction period. */
    uint8_t rejected_in_a_row;   /* Consecutive observations ignored as bad. */
    int64_t rejecting_since_us;  /* Uptime of the first of those. */
} clock_discipline_t;

typedef enum {
    CLOCK_OBSERVATION_ANCHORED,  /* First observation: the clock now starts at GPS time. */
    CLOCK_OBSERVATION_CORRECTED, /* Error accepted and being corrected gradually. */
    CLOCK_OBSERVATION_REJECTED,  /* Error too large; observation ignored. */
    CLOCK_OBSERVATION_STEPPED,   /* Rejected for too long; clock jumped to GPS time. */
} clock_observation_t;

/* UTC in microseconds at the given ESP32 uptime, or zero until synchronized. */
int64_t clock_discipline_utc_us(const clock_discipline_t *clock, int64_t uptime_us);

/* Feed one GPS observation: the GPS said it was utc_us at uptime_us.
 * error_us receives GPS minus the current estimate (zero for the first one). */
clock_observation_t clock_discipline_observe(clock_discipline_t *clock,
                                             int64_t uptime_us, int64_t utc_us,
                                             int64_t *error_us);
