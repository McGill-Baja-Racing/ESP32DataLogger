#include "time_sync.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Only this module owns the master-to-local clock relationship. */
static int64_t master_offset_us;
static bool synchronized;
static portMUX_TYPE sync_lock = portMUX_INITIALIZER_UNLOCKED;

void time_sync_update(uint64_t master_time_us)
{
    portENTER_CRITICAL(&sync_lock);
    master_offset_us = (int64_t)master_time_us - esp_timer_get_time();
    synchronized = true;
    portEXIT_CRITICAL(&sync_lock);
}

void time_sync_use_local_clock(void)
{
    portENTER_CRITICAL(&sync_lock);
    master_offset_us = 0;
    synchronized = true;
    portEXIT_CRITICAL(&sync_lock);
}

bool time_sync_get_master_time_us(int64_t *master_time_us)
{
    int64_t local_time_us = esp_timer_get_time();
    portENTER_CRITICAL(&sync_lock);
    bool valid = synchronized;
    int64_t offset_us = master_offset_us;
    portEXIT_CRITICAL(&sync_lock);
    if (valid && master_time_us) {
        *master_time_us = local_time_us + offset_us;
    }
    return valid;
}

int32_t time_sync_timestamp_ms(void)
{
    portENTER_CRITICAL(&sync_lock);
    int64_t offset_us = master_offset_us;
    portEXIT_CRITICAL(&sync_lock);
    return (int32_t)((esp_timer_get_time() + offset_us) / 1000);
}
