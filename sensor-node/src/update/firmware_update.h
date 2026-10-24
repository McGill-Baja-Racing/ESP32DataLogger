#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t firmware_update_init(void);

void firmware_update_report_identity(void);

void firmware_update_handle_command(const uint8_t *payload, uint8_t length);

void firmware_update_handle_data(const uint8_t *payload, uint8_t length);

void firmware_update_confirm(void);
