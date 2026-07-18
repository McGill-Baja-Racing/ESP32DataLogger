#include "time_sync.h"

#include "esp_timer.h"

/* Only this module owns the master-to-local clock relationship. */
static volatile int64_t master_offset_us;

void time_sync_update(uint64_t master_time_us)
{
    master_offset_us = (int64_t)master_time_us - esp_timer_get_time();
}

int32_t time_sync_timestamp_ms(void)
{
    return (int32_t)((esp_timer_get_time() + master_offset_us) / 1000);
}
