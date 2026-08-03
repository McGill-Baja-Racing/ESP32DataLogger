#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Replaces the clock offset using the newest master microsecond beacon. */
void time_sync_update(uint64_t master_time_us);

/* Returns the local sample time expressed on the master's millisecond clock. */
int32_t time_sync_timestamp_ms(void);
bool time_sync_is_valid(void);
bool time_sync_is_stale(int64_t maximum_age_us);
