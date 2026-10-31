#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Initialize this node's sensors, the sample queue, and sampler tasks.
 *
 * A sensor whose init fails does not fail this call; it is skipped and
 * retried on each sampler_start().
 *
 * @retval ESP_OK                Sampler is ready but idle until sampler_start().
 * @retval ESP_ERR_INVALID_SIZE  The registry has more than MAX_SENSORS sensors.
 * @retval ESP_ERR_NO_MEM        The queue or a task could not be created.
 */
esp_err_t sampler_init(void);

/**
 * @brief Start sampling. Called by main.c on CAN START.
 *
 * Clears queued samples, retries failed sensor inits, starts ready sensors,
 * and schedules their first sample for now. Sensors that still fail are
 * skipped. No-op if already active.
 */
void sampler_start(void);

/**
 * @brief Stop sampling and sending. Called by main.c on CAN STOP.
 *
 * Drops queued samples. Sensors stay initialized; there is no stop callback.
 * No-op if already stopped.
 */
void sampler_stop(void);

/**
 * @brief Drop stale samples. Called by main.c when the CAN bus goes bus-off.
 *
 * Empties the sample queue and counts each dropped sample in node health. On
 * the engine node it also clears buffered spark events. Sampling keeps running
 * if active.
 */
void sampler_discard_pending(void);

/**
 * @brief Whether the sampler is recording, between sampler_start() and sampler_stop().
 *
 * @return true while recording, false otherwise.
 */
bool sampler_is_active(void);
