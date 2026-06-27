#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RPM_SIGNAL_FUNCTION_MAX_LEN 16

typedef struct {
    bool enabled;
    bool preview_enabled;
    uint32_t can_id;
    uint16_t sample_rate_hz;
    uint8_t gpio;
    char function[RPM_SIGNAL_FUNCTION_MAX_LEN];
} rpm_signal_config_t;

void rpm_sampler_configure_signals(const rpm_signal_config_t *signals, size_t signal_count);
esp_err_t rpm_sampler_start(void);
