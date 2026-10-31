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
 * @brief Set up the CAN controller and start the receive and recovery tasks.
 *
 * Receives every frame on the bus. The handler runs on the receive task, one
 * frame at a time; frames that arrive while the queue (256) is full are lost.
 * The recovery task restarts the bus after bus-off.
 *
 * @param message_handler Called for each received frame.
 * @retval ESP_OK               CAN is running.
 * @retval ESP_ERR_INVALID_ARG  message_handler is NULL.
 * @retval ESP_ERR_NO_MEM       The queue, mutex or a task could not be created.
 * @return Other TWAI driver errors from setup.
 */
esp_err_t can_master_init(can_message_handler_t message_handler);

/**
 * @brief Send one standard-ID CAN frame and wait until it is on the bus.
 *
 * See transmit() in can_master.c. A failure is counted in master health,
 * except for a frame the driver already holds: that one is counted only if
 * the driver aborts it.
 *
 * @param id 11-bit CAN ID.
 * @param payload Copied; may be NULL when length is 0.
 * @param length Payload bytes, 0-8.
 * @retval ESP_OK               The frame was sent.
 * @retval ESP_ERR_INVALID_ARG  length > 8, or payload is NULL with length > 0.
 * @retval ESP_ERR_TIMEOUT      A wait gave up: another task was sending, or
 *                              the previous frame or this one was not sent.
 * @return Other TWAI driver errors, for example while the bus is off.
 */
esp_err_t can_master_send(uint32_t id, const uint8_t *payload, uint8_t length);

/**
 * @brief Tell all nodes to start sampling.
 *
 * Sends CAN_ID_START; see send_command_burst() in can_master.c.
 *
 * @return ESP_OK if every copy was sent, otherwise the first error. Other
 *         copies may still have gone out.
 */
esp_err_t can_master_start_nodes(void);

/**
 * @brief Tell all nodes to stop sampling.
 *
 * Sends CAN_ID_STOP; see send_command_burst() in can_master.c.
 *
 * @return ESP_OK if every copy was sent, otherwise the first error. Other
 *         copies may still have gone out.
 */
esp_err_t can_master_stop_nodes(void);

/**
 * @brief Number of received frames lost because the receive queue was full.
 *
 * @return Frames lost since boot; never reset.
 */
uint32_t can_master_rx_drop_count(void);

/**
 * @brief Read the CAN receive and transmit error counters.
 *
 * Both are set to 0 if the read fails.
 *
 * @param rx_errors Set to the receive error count.
 * @param tx_errors Set to the transmit error count.
 * @retval ESP_OK                 Counts were read.
 * @retval ESP_ERR_INVALID_ARG    A pointer is NULL.
 * @retval ESP_ERR_INVALID_STATE  can_master_init() has not set up CAN yet.
 * @return Other TWAI driver errors from the read.
 */
esp_err_t can_master_error_counts(uint32_t *rx_errors, uint32_t *tx_errors);
