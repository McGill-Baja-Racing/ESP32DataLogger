#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Replaces the clock offset using the newest master microsecond beacon. */
void time_sync_update(uint64_t master_time_us);

/* Uses the local timer as the time grid when running without a CAN master. */
void time_sync_use_local_clock(void);

/* Returns false until a master beacon (or local bench clock) is available. */
bool time_sync_get_master_time_us(int64_t *master_time_us);

/* Returns the local sample time expressed on the master's millisecond clock. */
int32_t time_sync_timestamp_ms(void);
