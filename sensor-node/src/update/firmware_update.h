#pragma once

#include <stdint.h>

void firmware_update_report_identity(void);

void firmware_update_handle_command(const uint8_t *payload, uint8_t length);
