#pragma once

#include "can/can_master.h"
#include "esp_err.h"

typedef void (*gps_sample_handler_t)(const can_message_t *sample);

/* Starts the GPS UART reader and reports valid RMC samples to the application. */
esp_err_t gps_receiver_start(gps_sample_handler_t sample_handler);
