#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define GPS_SIGNAL_FUNCTION_MAX_LEN 16

typedef struct {
    bool enabled;
    bool preview_enabled;
    uint32_t can_id;
    uint16_t sample_rate_hz;
    char function[GPS_SIGNAL_FUNCTION_MAX_LEN];
} gps_signal_config_t;

typedef struct {
    bool valid;
    bool has_location;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_cm;
    uint16_t speed_kph_x100;
    uint16_t course_deg_x100;
    uint16_t hdop_x100;
    uint8_t satellites;
    uint8_t fix_quality;
    int64_t timestamp_us;
    char utc_time[16];
    char utc_date[8];
} gps_fix_t;

void gps_configure_signals(const gps_signal_config_t *signals, size_t signal_count);
void gps_set_logging_enabled(bool enabled);
esp_err_t gps_start(void);
