#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol/app_protocol.h"

/**
 * @brief Count this reset and start the task that reports the node's health.
 *
 * Every NODE_STATUS_PERIOD_MS the task sends the status and diagnostics
 * frames and, while recording, an event frame for each code and subject
 * counted since the last report. Also counts HEALTH_BOOT with the reset
 * reason as subject. Call after can_node_init().
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The task could not be created.
 */
esp_err_t node_health_init(void);

/**
 * @brief Record the result of a sensor's init.
 *
 * Call once per sensor before node_health_init(). A failed init shows as
 * HEALTH_SENSOR_INIT in every status and is sent once as an event.
 *
 * @param index Sensor index in the registry; ignored from MAX_SENSORS up.
 * @param can_id CAN ID of the sensor's samples.
 * @param error Result of the sensor's init callback.
 */
void node_health_sensor_init(size_t index, uint32_t can_id, esp_err_t error);

/**
 * @brief Record the result of one sensor read.
 *
 * A period with failed reads shows as HEALTH_SENSOR_READ, or as
 * HEALTH_SENSOR_DEAD if no read succeeded. Failed reads are sent as events
 * with the last error.
 *
 * @param index Sensor index in the registry; ignored from MAX_SENSORS up.
 * @param error Result of the read callback.
 */
void node_health_sensor_read(size_t index, esp_err_t error);

/**
 * @brief Record amount occurrences of code.
 *
 * Safe from interrupts. Codes counted per subject, such as HEALTH_CAN_TX,
 * keep a separate count for each subject while counters last. Does nothing
 * when amount is 0.
 *
 * @param code What happened.
 * @param amount How many times.
 * @param subject What it happened to, such as a CAN ID; sent as one byte.
 * @param error Error that caused it, or ESP_OK.
 */
void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error);

/**
 * @brief Set or clear a fault that lasts, such as error passive.
 *
 * Safe from interrupts. Each onset also counts once, so a fault shorter than
 * one status period still shows.
 *
 * @param fault Fault code, below HEALTH_FAULT_COUNT.
 * @param active true while the fault lasts.
 */
void node_health_set(health_code_t fault, bool active);
