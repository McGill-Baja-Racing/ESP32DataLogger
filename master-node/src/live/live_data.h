#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"

#define LIVE_DATA_SIGNAL_COUNT 20
#define LIVE_DATA_LEASE_SECONDS 10

typedef struct {
    uint32_t can_id;
    const char *signal;
    const char *node;
    const char *units;
    uint16_t native_rate_hz;
} live_signal_metadata_t;

typedef struct {
    uint32_t can_id;
    int32_t value;
    uint32_t timestamp_ms;
    uint32_t sequence;
    bool valid;
} live_sample_t;

typedef enum {
    LIVE_DATA_OK,
    LIVE_DATA_DISABLED,
    LIVE_DATA_BUSY,
    LIVE_DATA_INVALID_TOKEN,
} live_data_result_t;

/**
 * @brief Set up with no viewer and an empty cache.
 *
 * @retval ESP_OK Always.
 */
esp_err_t live_data_init(void);

/**
 * @brief Get the table of known signals, also used by the CSV export.
 *
 * @param count Set to LIVE_DATA_SIGNAL_COUNT; may be NULL.
 * @return The table, in live_data_snapshot() order.
 */
const live_signal_metadata_t *live_data_signals(size_t *count);

/**
 * @brief Keep a frame as the latest value of its signal.
 *
 * Does nothing unless a viewer holds the lease. Ignores unknown IDs and
 * frames shorter than 8 bytes.
 *
 * @param message Sensor or GPS frame.
 */
void live_data_record(const can_message_t *message);

/**
 * @brief Give the live data lease to one viewer and clear the cache.
 *
 * The lease ends LIVE_DATA_LEASE_SECONDS after the last live_data_snapshot().
 *
 * @param token Set to the viewer's token, never 0.
 * @return LIVE_DATA_OK, BUSY if another viewer holds the lease, or DISABLED
 *         if token is NULL.
 */
live_data_result_t live_data_start(uint32_t *token);

/**
 * @brief Copy the latest value of every signal and renew the lease.
 *
 * @param token Token from live_data_start().
 * @param samples Filled in live_data_signals() order; valid is false for a
 *                signal with no frame yet.
 * @param capacity Size of samples; at least LIVE_DATA_SIGNAL_COUNT.
 * @param count Set to LIVE_DATA_SIGNAL_COUNT.
 * @return LIVE_DATA_OK, DISABLED if no lease is held or an argument is bad,
 *         or INVALID_TOKEN if another viewer holds the lease.
 */
live_data_result_t live_data_snapshot(uint32_t token, live_sample_t *samples,
                                     size_t capacity, size_t *count);

/**
 * @brief End the viewer's lease and clear the cache.
 *
 * @param token Token from live_data_start().
 * @return LIVE_DATA_OK, DISABLED if no lease is held, or INVALID_TOKEN if
 *         another viewer holds the lease.
 */
live_data_result_t live_data_stop(uint32_t token);

/**
 * @brief End any lease and clear the cache.
 */
void live_data_force_stop(void);

/**
 * @brief Check whether a viewer holds the lease.
 *
 * @return true if a lease is held and has not expired.
 */
bool live_data_is_enabled(void);
