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

/**
 * @brief Set up the CAN controller and start the receive and recovery tasks.
 *
 * Receives only master commands (0x0A0-0x0A3). Callbacks may be NULL. START,
 * STOP and beacon callbacks run on the receive task; on_bus_off runs on the
 * recovery task, which then restarts the bus. Does not send the boot report;
 * call can_node_report_state() after.
 *
 * @param callbacks Copied, so it may live on the caller's stack.
 * @param boot_reset_reason esp_reset_reason(), sent in the boot state report.
 * @retval ESP_OK               CAN is running.
 * @retval ESP_ERR_INVALID_ARG  callbacks is NULL.
 * @retval ESP_ERR_NO_MEM       The queue, mutex or a task could not be created.
 * @return Other TWAI driver errors from setup.
 */
esp_err_t can_node_init(const can_node_callbacks_t *callbacks,
                        uint8_t boot_reset_reason);

/**
 * @brief Send one standard-ID CAN frame and wait until it is on the bus.
 *
 * See transmit() in can_node.c. A failure is counted in node health as
 * HEALTH_CAN_TX, except for a frame the driver already holds: it still goes
 * out, or is dropped at bus-off, which counts as HEALTH_CAN_BUS_OFF instead.
 *
 * @param can_id 11-bit CAN ID.
 * @param payload Copied; may be NULL when length is 0.
 * @param length Payload bytes, 0-8.
 * @retval ESP_OK               The frame was sent.
 * @retval ESP_ERR_INVALID_ARG  length > 8, or payload is NULL with length > 0.
 * @retval ESP_ERR_TIMEOUT      A wait gave up: another task was sending, or
 *                              the previous frame or this one was not sent.
 * @return Other TWAI driver errors, for example while the bus is off.
 */
esp_err_t can_node_send(uint32_t can_id, const uint8_t *payload, uint8_t length);

/**
 * @brief Send one frame like can_node_send(), but succeed once the driver has it.
 *
 * See transmit() in can_node.c. A timeout after the driver took the frame is
 * not an error, but it may still block up to 200 ms. Use where a later send
 * is acceptable.
 *
 * @param can_id 11-bit CAN ID.
 * @param payload Copied; may be NULL when length is 0.
 * @param length Payload bytes, 0-8.
 * @return ESP_OK if the driver holds the frame, otherwise the can_node_send()
 *         error.
 */
esp_err_t can_node_queue(uint32_t can_id, const uint8_t *payload, uint8_t length);

/**
 * @brief Read the CAN transmit and receive error counters.
 *
 * Each is capped at 255. Both are 0 before can_node_init() or if the read
 * fails.
 *
 * @param tx_errors Set to the transmit error count.
 * @param rx_errors Set to the receive error count.
 */
void can_node_error_counts(uint8_t *tx_errors, uint8_t *rx_errors);

/**
 * @brief Record this node's state and report it to the master.
 *
 * Sends the 3-byte state frame on CAN_ID_NODE_STATE_BASE + NODE_ID: state,
 * reason, and the reset reason for NODE_STATE_REASON_BOOT (else 0). The
 * recorded state decides whether a later START, STOP or beacon calls the
 * start/stop callbacks. A failed send is logged and not retried.
 *
 * @param state State to record and send.
 * @param reason Why the state was reported.
 */
void can_node_report_state(node_state_t state, node_state_reason_t reason);
