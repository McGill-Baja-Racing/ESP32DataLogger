#pragma once

#include "can/can_master.h"
#include "esp_err.h"

typedef void (*gps_sample_handler_t)(const can_message_t *sample);

/**
 * @brief Set up the GPS UART and start the task that reads the receiver.
 *
 * The task first asks the receiver for RMC only at 5 Hz, then reads RMC
 * sentences. Each fix with a position and speed becomes three samples
 * (CAN_ID_GPS_SPEED, CAN_ID_GPS_LATITUDE, CAN_ID_GPS_LONGITUDE) on the master
 * clock in ms, and its UTC time goes to absolute_clock_observe_utc(). After
 * 3 s without bytes or without a fix it sets HEALTH_GPS_NO_DATA or
 * HEALTH_GPS_NO_FIX.
 *
 * @param sample_handler Called for each sample, on the GPS task.
 * @retval ESP_OK                GPS is running.
 * @retval ESP_ERR_INVALID_ARG   sample_handler is NULL.
 * @retval ESP_ERR_INVALID_STATE Already started.
 * @retval ESP_ERR_NO_MEM        The task could not be created.
 * @return Other UART driver errors from setup.
 */
esp_err_t gps_receiver_start(gps_sample_handler_t sample_handler);
