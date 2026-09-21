#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct sensor sensor_t;

/*
 * Common contract implemented by every sensor driver. The sampler owns
 * next_sample_us; the driver owns context and its hardware-specific contents.
 */
struct sensor {
    const char *name;
    uint32_t can_id;
    uint32_t period_us;
    int64_t next_sample_us;
    esp_err_t (*init)(sensor_t *sensor);
    void (*start)(sensor_t *sensor);
    /* Write value only on ESP_OK; failed reads are not transmitted. */
    esp_err_t (*read)(sensor_t *sensor, int32_t *value);
    void *context;
};

/* Returns the compile-time sensor list for the selected PlatformIO build. */
sensor_t *sensor_registry(size_t *count);
