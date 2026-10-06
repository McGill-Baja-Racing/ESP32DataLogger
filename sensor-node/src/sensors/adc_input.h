#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t adc_input_init(void);
esp_err_t adc_input_read_mv(uint8_t gpio, int *voltage_mv);
