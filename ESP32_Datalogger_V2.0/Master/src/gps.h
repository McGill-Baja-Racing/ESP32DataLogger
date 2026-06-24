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

void gps_configure_signals(const gps_signal_config_t *signals, size_t signal_count);
void gps_set_logging_enabled(bool enabled);
esp_err_t gps_start(void);
