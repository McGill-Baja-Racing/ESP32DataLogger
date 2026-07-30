#pragma once

#include <stdbool.h>

#include "can/can_master.h"
#include "esp_err.h"

/* Bit N enables liveness monitoring for node ID N. */
#define MASTER_EXPECTED_NODE_MASK ((1U << 4) | (1U << 5))

esp_err_t node_registry_init(void);
bool node_registry_is_state_frame(const can_message_t *message);
void node_registry_update(const can_message_t *message);
void node_registry_set_monitoring(bool enabled);
void node_registry_record_sensor_frame(const can_message_t *message);
const char *node_registry_state_name(uint8_t node_id);
