#pragma once

#include <stdbool.h>

#include "can/can_master.h"
#include "esp_err.h"

/* Bit N enables liveness monitoring for node ID N. */
#define MASTER_EXPECTED_NODE_MASK ((1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 7) | (1U << 8))

/**
 * @brief Start the task that marks silent expected nodes offline.
 *
 * A node goes offline after 3 s without a sensor or health frame; it is logged
 * and counted as HEALTH_NODE_OFFLINE. Only while monitoring is on.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  Creating the task failed.
 */
esp_err_t node_registry_init(void);

/**
 * @brief Check whether a frame is a node state report.
 *
 * @param message Received frame.
 * @return true for CAN_ID_NODE_STATE_BASE + node ID.
 */
bool node_registry_is_state_frame(const can_message_t *message);

/**
 * @brief Store a node's state report and log a change.
 *
 * Ignores frames shorter than 2 bytes. It does not count as a sign of life.
 *
 * @param message Frame accepted by node_registry_is_state_frame().
 */
void node_registry_update(const can_message_t *message);

/**
 * @brief Turn offline monitoring on at recording start, off at stop.
 *
 * Either way, every expected node resets to online with a full 3 s grace and no
 * data seen. While off, frames are ignored.
 *
 * @param enabled true while recording.
 */
void node_registry_set_monitoring(bool enabled);

/**
 * @brief Mark the frame's node alive and sending data. See record_frame().
 *
 * @param message Received sensor frame.
 */
void node_registry_record_sensor_frame(const can_message_t *message);

/**
 * @brief Mark a node alive without sensor data. See record_frame().
 *
 * @param node_id Node that sent the frame.
 */
void node_registry_record_health_frame(uint8_t node_id);

/**
 * @brief Describe a node's state for /api/status.
 *
 * @param node_id Node ID.
 * @return "active" (data in the last 3 s), "waiting" (alive, no data),
 *         "offline", "off" (not recording), "disabled" (not expected) or
 *         "unknown" (invalid ID).
 */
const char *node_registry_state_name(uint8_t node_id);
