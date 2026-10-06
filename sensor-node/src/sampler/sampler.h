#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Initializes selected sensors, the sample queue, and sampler tasks. */
esp_err_t sampler_init(void);

/* START/STOP lifecycle called from the CAN command callbacks in main.c. */
void sampler_start(void);
void sampler_stop(void);

/* Clears stale queued data without changing the active state (bus-off use). */
void sampler_discard_pending(void);
bool sampler_is_active(void);
