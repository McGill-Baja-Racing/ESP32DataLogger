#pragma once

#include <stdbool.h>

#include "can/can_master.h"
#include "esp_err.h"

/* Bit N enables liveness monitoring for node ID N. */
#define MASTER_EXPECTED_NODE_MASK ((1U << 1) | (1U << 3) | (1U << 4) | (1U << 5))

/**
 * @brief Start the task that marks expected nodes offline.
 *
 * Every 250 ms it marks a node offline after 3 s without a sensor or health
 * frame, logs it, and counts HEALTH_NODE_OFFLINE. Only while monitoring is on.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The task could not be created.
 */
esp_err_t node_registry_init(void);

/**
 * @brief Check whether a frame is a node state report.
 *
 * @param message Received frame.
 * @return true for CAN_ID_NODE_STATE_BASE + node ID, nodes 1 and up.
 */
bool node_registry_is_state_frame(const can_message_t *message);

/**
 * @brief Store a node's state report and log it if it changed.
 *
 * Reads state, reason and the optional reset reason. Ignores frames shorter
 * than 2 bytes. A state report does not count as a sign of life for the
 * offline check.
 *
 * @param message Frame accepted by node_registry_is_state_frame().
 */
void node_registry_update(const can_message_t *message);

/**
 * @brief Turn offline monitoring on at recording start, off at stop.
 *
 * Either way, every expected node starts fresh: online, with a full 3 s
 * before it can go offline, and no sensor data seen yet. While off, frames
 * are ignored and no node goes offline.
 *
 * @param enabled true while recording.
 */
void node_registry_set_monitoring(bool enabled);

/**
 * @brief Note a sensor frame: its node is alive and sending data.
 *
 * Finds the node from the CAN ID; see record_frame() in node_registry.c.
 *
 * @param message Received sensor frame.
 */
void node_registry_record_sensor_frame(const can_message_t *message);

/**
 * @brief Note a health frame: the node is alive, but this is not sensor data.
 *
 * See record_frame() in node_registry.c.
 *
 * @param node_id Node that sent the frame.
 */
void node_registry_record_health_frame(uint8_t node_id);

/**
 * @brief Describe a node for /api/status.
 *
 * @param node_id Node ID.
 * @return "active" if sensor data arrived in the last 3 s, "waiting" if the
 *         node is alive but sends no data, "offline" after 3 s of silence,
 *         "off" when not recording, "disabled" if the node is not in
 *         MASTER_EXPECTED_NODE_MASK, or "unknown" for an invalid ID.
 */
const char *node_registry_state_name(uint8_t node_id);
