#include "time/absolute_clock.h"
#include "time/clock_discipline.h"
#include "time/gps_time.h"
#include <inttypes.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The GPS task writes the clock while logger and web tasks read it. */
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static clock_discipline_t discipline;

static int64_t utc_us_at(int64_t uptime_us)
{
    portENTER_CRITICAL(&clock_lock);
    int64_t utc_us = clock_discipline_utc_us(&discipline, uptime_us);
    portEXIT_CRITICAL(&clock_lock);
    return utc_us;
}

int64_t absolute_clock_sample_utc(uint32_t sample_ms)
{
    int64_t now_us = esp_timer_get_time();
    int64_t utc_us = utc_us_at(now_us);
    if (!utc_us) return 0;
    return gps_time_sample_utc(utc_us / 1000, (uint64_t)(now_us / 1000), sample_ms);
}

int64_t absolute_clock_now_utc(void)
{
    return utc_us_at(esp_timer_get_time()) / 1000;
}

/* Give the system clock UTC so files on the SD card get real dates. */
static void set_system_clock(void)
{
    int64_t now_ms = absolute_clock_now_utc();
    struct timeval system_time = {
        .tv_sec = (time_t)(now_ms / 1000),
        .tv_usec = (suseconds_t)((now_ms % 1000) * 1000),
    };
    if (settimeofday(&system_time, NULL) != 0) {
        ESP_LOGW("AbsoluteClock", "Unable to set system UTC clock");
    }
}

void absolute_clock_observe_utc(int64_t utc_ms, int64_t uptime_us)
{
    if (utc_ms <= 0) return;
    int64_t error_us;
    portENTER_CRITICAL(&clock_lock);
    clock_observation_t result =
        clock_discipline_observe(&discipline, uptime_us, utc_ms * 1000, &error_us);
    portEXIT_CRITICAL(&clock_lock);

    switch (result) {
        case CLOCK_OBSERVATION_ANCHORED:
            set_system_clock();
            ESP_LOGI("AbsoluteClock", "UTC synchronized from GPS RMC");
            break;
        case CLOCK_OBSERVATION_CORRECTED:
            ESP_LOGD("AbsoluteClock", "Correcting GPS UTC error of %+" PRId64 " us",
                     error_us);
            break;
        case CLOCK_OBSERVATION_REJECTED:
            ESP_LOGW("AbsoluteClock", "Ignoring GPS UTC %+" PRId64 " us from estimate",
                     error_us);
            break;
        case CLOCK_OBSERVATION_STEPPED:
            ESP_LOGW("AbsoluteClock", "UTC jumped %+" PRId64 " us after lasting "
                     "disagreement with GPS", error_us);
            break;
    }
}
