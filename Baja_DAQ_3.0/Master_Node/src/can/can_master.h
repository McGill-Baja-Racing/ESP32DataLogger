#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint64_t data;
} can_message_t;

typedef void (*can_message_handler_t)(const can_message_t *message);

/* Initializes TWAI plus receive-dispatch and bus-off recovery tasks. */
esp_err_t can_master_init(can_message_handler_t message_handler);
esp_err_t can_master_send(uint32_t id, const uint8_t *payload, uint8_t length);
esp_err_t can_master_start_nodes(void);
esp_err_t can_master_stop_nodes(void);
uint32_t can_master_rx_drop_count(void);
