#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"

#define LIVE_DATA_SIGNAL_COUNT 9
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

esp_err_t live_data_init(void);
const live_signal_metadata_t *live_data_signals(size_t *count);
void live_data_record(const can_message_t *message);
live_data_result_t live_data_start(uint32_t *token);
live_data_result_t live_data_snapshot(uint32_t token, live_sample_t *samples,
                                     size_t capacity, size_t *count);
live_data_result_t live_data_stop(uint32_t token);
void live_data_force_stop(void);
bool live_data_is_enabled(void);
