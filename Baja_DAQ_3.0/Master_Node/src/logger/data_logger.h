#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"

typedef enum {
    LOGGER_IDLE,
    LOGGER_RUNNING,
    LOGGER_STOPPING,
} logger_state_t;

esp_err_t data_logger_init(void);
bool data_logger_start(void);
bool data_logger_stop(void);
void data_logger_enqueue(const can_message_t *message);
logger_state_t data_logger_state(void);
const char *data_logger_state_name(void);
const char *data_logger_path(void);
uint32_t data_logger_drop_count(void);
