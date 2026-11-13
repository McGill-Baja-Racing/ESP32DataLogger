#pragma once

#include "can/can_master.h"
#include "esp_err.h"

typedef void (*gps_sample_handler_t)(const can_message_t *sample);

/**
 * @brief Set up the GPS UART and start the task that reads the receiver.
 *
 * Each fix with position and speed becomes speed, latitude and longitude
 * samples on the master clock, and its UTC time goes to
 * absolute_clock_observe_utc(). After 3 s without bytes or without a fix it
 * counts HEALTH_GPS_NO_DATA or HEALTH_GPS_NO_FIX.
 *
 * @param sample_handler Called for each sample, on the GPS task.
 * @retval ESP_OK GPS is running.
 * @retval ESP_ERR_INVALID_ARG sample_handler is NULL.
 * @retval ESP_ERR_INVALID_STATE Already started.
 * @retval ESP_ERR_NO_MEM The task could not be created.
 * @return Other UART driver errors from setup.
 */
esp_err_t gps_receiver_start(gps_sample_handler_t sample_handler);
