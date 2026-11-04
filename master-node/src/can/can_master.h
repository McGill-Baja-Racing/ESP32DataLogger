#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifndef MASTER_CAN_ENABLED
#define MASTER_CAN_ENABLED 1
#endif

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint64_t data;
} can_message_t;

typedef void (*can_message_handler_t)(const can_message_t *message);

/**
 * @brief Set up the CAN controller and start its receive and recovery tasks.
 *
 * The handler runs on the receive task, one frame at a time. Frames are lost
 * while the 256-frame queue is full.
 *
 * @param message_handler Called for each received frame.
 * @retval ESP_OK               CAN is running.
 * @retval ESP_ERR_INVALID_ARG  message_handler is NULL.
 * @retval ESP_ERR_NO_MEM       Creating the queue, mutex or a task failed.
 * @return Other TWAI driver errors.
 */
esp_err_t can_master_init(can_message_handler_t message_handler);

/**
 * @brief Send one standard-ID CAN frame; a failure counts as HEALTH_CAN_TX.
 *
 * Safe from any task. Waits up to 100 ms for the frame to go out; one still
 * pending then goes out later. Blocks up to 400 ms.
 *
 * @param id 11-bit CAN ID.
 * @param payload Copied; may be NULL when length is 0.
 * @param length Payload bytes, 0-8.
 * @retval ESP_OK               The driver holds the frame.
 * @retval ESP_ERR_INVALID_ARG  length > 8, or payload is NULL with length > 0.
 * @retval ESP_ERR_TIMEOUT      Another send or the previous frame timed out.
 * @return Other TWAI driver errors, for example while the bus is off.
 */
esp_err_t can_master_send(uint32_t id, const uint8_t *payload, uint8_t length);

/**
 * @brief Tell all nodes to start sampling. See send_command_burst().
 *
 * @return ESP_OK if every copy was sent, else the first error.
 */
esp_err_t can_master_start_nodes(void);

/**
 * @brief Tell all nodes to stop sampling. See send_command_burst().
 *
 * @return ESP_OK if every copy was sent, else the first error.
 */
esp_err_t can_master_stop_nodes(void);

/**
 * @brief Count received frames lost because the receive queue was full.
 *
 * @return Frames lost since boot.
 */
uint32_t can_master_rx_drop_count(void);

/**
 * @brief Read the CAN receive and transmit error counters.
 *
 * Both are set to 0 if the read fails.
 *
 * @param rx_errors Receive error count.
 * @param tx_errors Transmit error count.
 * @retval ESP_OK                 Counts were read.
 * @retval ESP_ERR_INVALID_ARG    A pointer is NULL.
 * @retval ESP_ERR_INVALID_STATE  can_master_init() has not run.
 * @return Other TWAI driver errors.
 */
esp_err_t can_master_error_counts(uint32_t *rx_errors, uint32_t *tx_errors);
