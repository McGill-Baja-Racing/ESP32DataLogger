#pragma once

#include <stdbool.h>

#include "can/can_master.h"

bool node_registry_is_state_frame(const can_message_t *message);
void node_registry_update(const can_message_t *message);
const char *node_registry_state_name(uint8_t node_id);
