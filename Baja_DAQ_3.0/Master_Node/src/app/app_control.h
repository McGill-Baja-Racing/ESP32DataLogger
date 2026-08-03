#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "logger/data_logger.h"

typedef enum {
    APP_CONTROL_OK,
    APP_CONTROL_CONFLICT,
    APP_CONTROL_BUSY,
    APP_CONTROL_FAILED,
} app_control_result_t;

typedef struct {
    logger_state_t logger_state;
    const char *current_file;
    uint32_t can_drops;
    uint32_t log_drops;
    bool live_enabled;
    const char *node_1;
    const char *node_4;
    const char *node_5;
    const char *node_6;
} app_status_t;

esp_err_t app_control_init(void);
app_control_result_t app_control_start_logging(void);
app_control_result_t app_control_stop_logging(void);
app_control_result_t app_control_start_live_data(uint32_t *token);
void app_control_get_status(app_status_t *status);
void app_control_print_status(void);
