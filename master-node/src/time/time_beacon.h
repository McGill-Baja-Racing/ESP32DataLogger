#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Start the task that broadcasts the master clock to the nodes.
 *
 * See beacon_task() in time_beacon.c.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The task could not be created.
 */
esp_err_t time_beacon_start(void);

/**
 * @brief Set the recording flag in every following beacon.
 *
 * A node whose state disagrees with the flag starts or stops, so one that
 * missed START or STOP, or rebooted, catches up within one beacon.
 *
 * @param recording true while a run is being logged.
 */
void time_beacon_set_recording(bool recording);
