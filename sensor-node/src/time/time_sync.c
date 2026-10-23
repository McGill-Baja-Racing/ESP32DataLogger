#include "time_sync.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Only this module owns the master-to-local clock relationship. */
static int64_t master_offset_us;
static int64_t last_update_us;
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;

void time_sync_update(uint64_t master_time_us)
{
    int64_t now = esp_timer_get_time();
    int64_t offset = (int64_t)master_time_us - now;
    portENTER_CRITICAL(&clock_lock);
    master_offset_us = offset;
    last_update_us = now;
    portEXIT_CRITICAL(&clock_lock);
}

bool time_sync_is_current(uint32_t max_age_ms)
{
    portENTER_CRITICAL(&clock_lock);
    int64_t last = last_update_us;
    portEXIT_CRITICAL(&clock_lock);
    return last != 0 && esp_timer_get_time() - last <= (int64_t)max_age_ms * 1000;
}

int32_t time_sync_timestamp_ms_at(int64_t local_us)
{
    portENTER_CRITICAL(&clock_lock);
    int64_t offset = master_offset_us;
    portEXIT_CRITICAL(&clock_lock);
    return (int32_t)((local_us + offset) / 1000);
}

int32_t time_sync_timestamp_ms(void)
{
    return time_sync_timestamp_ms_at(esp_timer_get_time());
}
