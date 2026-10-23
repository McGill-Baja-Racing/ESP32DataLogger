#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol/app_protocol.h"

/* Counts this reset and starts the task that sends the status and diagnostics
 * frames every second and, while recording, an event frame for each counted
 * fault. Call after can_node_init(). */
esp_err_t node_health_init(void);

/* Call once per sensor before node_health_init(), with the init result. */
void node_health_sensor_init(size_t index, uint32_t can_id, esp_err_t error);
void node_health_sensor_read(size_t index, esp_err_t error);

/* Records amount occurrences of code. Safe from interrupts. */
void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error);

/* Sets or clears a fault that lasts, such as error passive. Each onset also
 * counts once. Safe from interrupts. */
void node_health_set(health_code_t fault, bool active);
