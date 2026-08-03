#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "can/can_master.h"
#include "esp_err.h"
#define DIAGNOSTIC_HISTORY_COUNT 20
typedef struct { uint8_t node_id; uint16_t code; uint8_t flags; uint8_t count; uint32_t timestamp_ms; } diagnostic_event_t;
esp_err_t diagnostic_registry_init(void);
bool diagnostic_registry_is_frame(const can_message_t *message);
bool diagnostic_registry_update(const can_message_t *message, diagnostic_event_t *event);
size_t diagnostic_registry_snapshot(diagnostic_event_t *events, size_t capacity);
const char *diagnostic_code_name(uint16_t code);
const char *diagnostic_severity_name(uint8_t flags);
