#include "time_sync.h"

#include "esp_timer.h"

/* Only this module owns the master-to-local clock relationship. */
static volatile int64_t master_offset_us;
static volatile bool valid;
static volatile int64_t last_beacon_us;

void time_sync_update(uint64_t master_time_us)
{
    master_offset_us = (int64_t)master_time_us - esp_timer_get_time();
    valid = true;
    last_beacon_us = esp_timer_get_time();
}
bool time_sync_is_valid(void){return valid;}
bool time_sync_is_stale(int64_t maximum_age_us){return !valid||esp_timer_get_time()-last_beacon_us>maximum_age_us;}

int32_t time_sync_timestamp_ms(void)
{
    return (int32_t)((esp_timer_get_time() + master_offset_us) / 1000);
}
