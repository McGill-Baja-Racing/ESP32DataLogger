#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"
#include "protocol/app_protocol.h"

/* Starts the task that logs the master's own status and events as node 0. */
esp_err_t node_health_init(void);

#define NODE_HEALTH_TEXT_MAX 320

bool node_health_is_frame(const can_message_t *message);
uint8_t node_health_node_id(const can_message_t *message);

/* For a health frame ID, node 0 included: sets *node, writes a
 * one-line description without commas to text and returns the signal name.
 * Returns NULL for any other ID. */
const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size);

/* Queues a health frame for printing and logs it like a sample.
 * Counts node events that were lost on the way or refused by the logger.
 * Returns whether the logger accepted it. */
bool node_health_handle_frame(const can_message_t *message);

/* Records amount occurrences of code. Safe from interrupts. */
void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error);

/* Sets or clears a fault that lasts, such as no GPS fix. Each onset also
 * counts once. Safe from interrupts. */
void node_health_set(health_code_t fault, bool active);
