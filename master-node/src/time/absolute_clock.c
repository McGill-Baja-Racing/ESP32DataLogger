#include "time/absolute_clock.h"
#include "time/gps_time.h"
#include <sys/time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t utc_offset_ms;
static bool synchronized;

int64_t absolute_clock_sample_utc(uint32_t sample_ms)
{
    portENTER_CRITICAL(&clock_lock);
    bool valid = synchronized;
    int64_t offset = utc_offset_ms;
    portEXIT_CRITICAL(&clock_lock);
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    return valid ? gps_time_sample_utc((int64_t)now + offset, now, sample_ms) : 0;
}

int64_t absolute_clock_now_utc(void)
{
    portENTER_CRITICAL(&clock_lock);
    bool valid = synchronized;
    int64_t offset = utc_offset_ms;
    portEXIT_CRITICAL(&clock_lock);
    return valid ? esp_timer_get_time() / 1000 + offset : 0;
}

void absolute_clock_observe_utc(int64_t utc_ms)
{
    if (utc_ms <= 0) return;
    int64_t now = esp_timer_get_time() / 1000;
    /* Anchor once per boot so serial jitter cannot step sample time. */
    portENTER_CRITICAL(&clock_lock);
    bool first = !synchronized;
    if (first) { utc_offset_ms = utc_ms - now; synchronized = true; }
    portEXIT_CRITICAL(&clock_lock);
    if (first) {
        struct timeval system_time = {
            .tv_sec = (time_t)(utc_ms / 1000),
            .tv_usec = (suseconds_t)((utc_ms % 1000) * 1000),
        };
        if (settimeofday(&system_time, NULL) != 0) {
            ESP_LOGW("AbsoluteClock", "Unable to set system UTC clock");
        }
        ESP_LOGI("AbsoluteClock", "UTC synchronized from GPS RMC");
    }
}
