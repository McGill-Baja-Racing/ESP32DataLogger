#pragma once

#include <stdbool.h>

#include "can/can_master.h"
#include "esp_err.h"

bool firmware_update_is_identity_frame(const can_message_t *message);
void firmware_update_handle_identity(const can_message_t *message);
esp_err_t firmware_update_request_identity(void);
void firmware_update_print_identity(void);
