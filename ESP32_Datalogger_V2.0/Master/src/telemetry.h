#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t telemetry_start(void);
void telemetry_submit_can_frame(uint32_t id, uint8_t dlc, uint64_t data);
