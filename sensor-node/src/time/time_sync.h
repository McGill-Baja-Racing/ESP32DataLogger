#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Set the clock offset from a master time beacon.
 *
 * Replaces the offset outright, with no smoothing, so timestamps can step
 * slightly at each beacon. Called by can_node for every beacon, about every
 * 100 ms.
 *
 * @param master_time_us Master clock in microseconds, recording flag removed.
 */
void time_sync_update(uint64_t master_time_us);

/**
 * @brief Get the current time on the master's clock, in milliseconds.
 *
 * Before the first beacon it returns local time since boot instead.
 *
 * @return Master time in ms.
 */
int32_t time_sync_timestamp_ms(void);

/**
 * @brief Convert a local esp_timer_get_time() reading to master time in ms.
 *
 * Uses the current offset, so convert soon after capture: a beacon in
 * between shifts the result by the clock drift since the last one. Before
 * the first beacon it returns local time since boot instead.
 *
 * @param local_us Local time in microseconds since boot.
 * @return Master time in ms.
 */
int32_t time_sync_timestamp_ms_at(int64_t local_us);

/**
 * @brief Check whether timestamps are on the master's clock.
 *
 * @param max_age_ms Oldest beacon age still accepted.
 * @return true if a beacon arrived within max_age_ms, false otherwise or if
 *         none has arrived yet.
 */
bool time_sync_is_current(uint32_t max_age_ms);
