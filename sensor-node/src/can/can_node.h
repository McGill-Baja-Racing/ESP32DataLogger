#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "protocol/app_protocol.h"

/* Events decoded by the CAN module and handled by the application modules. */
typedef struct {
    void (*on_start)(void);
    void (*on_stop)(void);
    void (*on_time_beacon)(uint64_t master_time_us);
    void (*on_bus_off)(void);
} can_node_callbacks_t;

/* Initializes TWAI and starts the command-dispatch and recovery tasks. */
esp_err_t can_node_init(const can_node_callbacks_t *callbacks,
                        uint8_t boot_reset_reason);

/* Sends one standard CAN frame. The payload is copied by the TWAI driver. */
esp_err_t can_node_send(uint32_t can_id, const uint8_t *payload, uint8_t length);

/* Sends the three-byte state acknowledgement defined in app_protocol.h. */
void can_node_report_state(node_state_t state, node_state_reason_t reason);
