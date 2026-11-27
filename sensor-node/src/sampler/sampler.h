#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Init this node's sensors and start the sampler tasks.
 *
 * A sensor whose init fails does not fail this call; it is never read.
 *
 * @retval ESP_OK                Sampler is ready; samples are sent only after sampler_start().
 * @retval ESP_ERR_NO_MEM        The queue or a task could not be created.
 */
esp_err_t sampler_init(void);

/**
 * @brief Clear queued samples and start sampling. Does nothing if already active.
 */
void sampler_start(void);

/**
 * @brief Stop sampling and drop queued samples. Does nothing if already stopped.
 */
void sampler_stop(void);

/**
 * @brief Drop queued samples, for example on bus-off.
 *
 * Counts each as dropped in node health. Sampling keeps running.
 */
void sampler_discard_pending(void);

/**
 * @brief Check whether the sampler is recording.
 *
 * @return true between sampler_start() and sampler_stop().
 */
bool sampler_is_active(void);
