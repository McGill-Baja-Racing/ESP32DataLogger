#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Set the clock offset from a master time beacon.
 *
 * The offset is replaced, not smoothed, so timestamps can step at each beacon.
 *
 * @param master_time_us Master clock in us, without the recording flag.
 */
void time_sync_update(uint64_t master_time_us);

/**
 * @brief Get the current master time. See time_sync_timestamp_ms_at().
 *
 * @return Master time in ms.
 */
int32_t time_sync_timestamp_ms(void);

/**
 * @brief Convert a local time to master time.
 *
 * Convert soon after capture, since a beacon in between shifts the result.
 * Before the first beacon it returns local time.
 *
 * @param local_us Local time in us, from esp_timer_get_time().
 * @return Master time in ms.
 */
int32_t time_sync_timestamp_ms_at(int64_t local_us);

/**
 * @brief Check whether timestamps are on the master's clock.
 *
 * @param max_age_ms Oldest beacon age accepted.
 * @return true if a beacon arrived within max_age_ms.
 */
bool time_sync_is_current(uint32_t max_age_ms);
