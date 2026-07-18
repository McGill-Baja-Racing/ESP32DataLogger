#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool (*start)(void);
    bool (*stop)(void);
    void (*status)(void);
} serial_console_callbacks_t;

esp_err_t serial_console_start(const serial_console_callbacks_t *callbacks);
