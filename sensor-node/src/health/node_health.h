#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol/app_protocol.h"
#include "protocol/health_counters.h"

/**
 * @brief Count this reset and start the health reporting task.
 *
 * Each period the task sends status and diagnostics, and while recording the
 * events counted since the last report. Call after can_node_init().
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The task could not be created.
 */
esp_err_t node_health_init(void);

/**
 * @brief Record the result of a sensor's init.
 *
 * Call before node_health_init(). A failure shows in every status and is sent
 * once as an event.
 *
 * @param index Sensor index, below MAX_SENSORS.
 * @param can_id CAN ID of the sensor's samples. Sensors past NODE_SENSOR_SLOTS
 *               do not show in the status.
 * @param error Result of the init.
 */
void node_health_sensor_init(size_t index, uint32_t can_id, esp_err_t error);

/**
 * @brief Record the result of one sensor read.
 *
 * Failed reads feed the sensor's status and are sent as events.
 *
 * @param index Sensor index, below MAX_SENSORS.
 * @param error Result of the read.
 */
void node_health_sensor_read(size_t index, esp_err_t error);
