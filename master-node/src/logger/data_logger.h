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

typedef enum {
    DATA_LOGGER_START_OK,
    DATA_LOGGER_START_ACTIVE,
    DATA_LOGGER_START_INVALID_NAME,
    DATA_LOGGER_START_EXISTS,
    DATA_LOGGER_START_OPEN_FAILED,
} data_logger_start_result_t;

typedef enum {
    DATA_LOGGER_RENAME_OK,
    DATA_LOGGER_RENAME_INVALID_NAME,
    DATA_LOGGER_RENAME_ACTIVE,
    DATA_LOGGER_RENAME_NOT_FOUND,
    DATA_LOGGER_RENAME_EXISTS,
    DATA_LOGGER_RENAME_FAILED,
} data_logger_rename_result_t;

#define DATA_LOGGER_FILENAME_MAX 44

esp_err_t data_logger_init(void);
data_logger_start_result_t data_logger_start(const char *filename);
bool data_logger_stop(void);
bool data_logger_valid_filename(const char *filename);
data_logger_rename_result_t data_logger_rename(const char *old_filename,
                                               const char *new_filename);
void data_logger_enqueue(const can_message_t *message);
logger_state_t data_logger_state(void);
const char *data_logger_state_name(void);
const char *data_logger_path(void);
uint32_t data_logger_drop_count(void);
