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
    void (*on_update_command)(const uint8_t *payload, uint8_t length);
    void (*on_update_data)(const uint8_t *payload, uint8_t length);
} can_node_callbacks_t;

/**
 * @brief Start the CAN controller and its receive and recovery tasks.
 *
 * Receives only master commands (0x0A0-0x0A7). Callbacks may be NULL.
 * on_bus_off runs on the recovery task, the others on the receive task.
 * on_update_command gets only commands for this node or all nodes, and the
 * payload pointers are valid only during the call. Does not send the boot
 * report; call can_node_report_state() after.
 *
 * @param callbacks Copied.
 * @param boot_reset_reason Sent in the boot state report.
 * @retval ESP_OK               CAN is running.
 * @retval ESP_ERR_INVALID_ARG  callbacks is NULL.
 * @retval ESP_ERR_NO_MEM       A queue, mutex or task could not be created.
 * @return Other TWAI driver errors.
 */
esp_err_t can_node_init(const can_node_callbacks_t *callbacks,
                        uint8_t boot_reset_reason);

/**
 * @brief Send one standard-ID CAN frame; a failure counts as HEALTH_CAN_TX.
 *
 * Safe from any task. Waits up to 50 ms for the frame to go out; one still
 * pending then goes out later. Blocks up to 200 ms.
 *
 * @param can_id 11-bit CAN ID.
 * @param payload Copied; may be NULL when length is 0.
 * @param length Payload bytes, 0-8.
 * @retval ESP_OK               The driver holds the frame.
 * @retval ESP_ERR_INVALID_ARG  length > 8, or payload is NULL with length > 0.
 * @retval ESP_ERR_TIMEOUT      Another send or the previous frame timed out.
 * @return Other TWAI driver errors, for example while the bus is off.
 */
esp_err_t can_node_send(uint32_t can_id, const uint8_t *payload, uint8_t length);

/**
 * @brief Wait until the last sent frame has left the controller.
 *
 * @param timeout_ms Longest wait.
 * @return ESP_OK once nothing is pending, otherwise the wait error.
 */
esp_err_t can_node_flush(uint32_t timeout_ms);

/**
 * @brief Read the CAN error counters, capped at 255.
 *
 * Both are 0 before can_node_init() or if the read fails.
 *
 * @param tx_errors Transmit error count.
 * @param rx_errors Receive error count.
 */
void can_node_error_counts(uint8_t *tx_errors, uint8_t *rx_errors);

/**
 * @brief Record this node's state and report it to the master.
 *
 * The recorded state decides whether a later START, STOP or beacon calls the
 * callbacks. A failed send is logged, not retried.
 *
 * @param state New state.
 * @param reason Why it is reported.
 */
void can_node_report_state(node_state_t state, node_state_reason_t reason);

/**
 * @brief Get the state last recorded by can_node_report_state().
 */
node_state_t can_node_state(void);
