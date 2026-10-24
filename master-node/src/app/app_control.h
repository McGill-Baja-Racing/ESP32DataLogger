#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "aim/aim_adapter.h"
#include "logger/data_logger.h"

typedef enum {
    APP_CONTROL_OK,
    APP_CONTROL_CONFLICT,
    APP_CONTROL_BUSY,
    APP_CONTROL_INVALID_FILENAME,
    APP_CONTROL_FILENAME_EXISTS,
    APP_CONTROL_FAILED,
} app_control_result_t;

typedef struct {
    logger_state_t logger_state;
    const char *current_file;
    uint32_t can_drops;
    uint32_t can_rx_errors;
    uint32_t can_tx_errors;
    bool can_errors_available;
    uint32_t log_drops;
    bool live_enabled;
    aim_status_t aim;
    const char *node_1;
    const char *node_3;
    const char *node_4;
    const char *node_5;
    const char *node_6;
    const char *node_7;
    const char *node_8;
} app_status_t;

/**
 * @brief Create the mutex that keeps start, stop and live data from racing.
 *
 * @retval ESP_OK          Ready.
 * @retval ESP_ERR_NO_MEM  The mutex could not be created.
 */
esp_err_t app_control_init(void);

/**
 * @brief Open a new log file, then tell the nodes to start sampling.
 *
 * A failed START broadcast is logged but still returns APP_CONTROL_OK, since
 * the time beacon also starts nodes.
 *
 * @param filename Name on the SD card, or NULL for a default name; see
 *                 data_logger_prepare().
 * @return APP_CONTROL_OK, or why it did not start: not idle (CONFLICT), bad
 *         name (INVALID_FILENAME), file exists (FILENAME_EXISTS), or file
 *         creation failed or the lock was held over 1 s (FAILED).
 */
app_control_result_t app_control_start_logging(const char *filename);

/**
 * @brief End live data, stop the logger, then tell the nodes to stop.
 *
 * Returns once stopping begins; see data_logger_stop(). A failed STOP broadcast
 * is logged but still returns APP_CONTROL_OK.
 *
 * @return APP_CONTROL_OK, CONFLICT if not running, or FAILED if the lock was
 *         held over 1 s or the logger refused.
 */
app_control_result_t app_control_stop_logging(void);

/**
 * @brief Start the firmware update task for sensor nodes and the master.
 *
 * @return APP_CONTROL_OK, CONFLICT unless the logger is idle, BUSY if an
 *         update is running, or FAILED if the lock was held for over 1 s or
 *         the task could not start.
 */
app_control_result_t app_control_update_firmware(void);

/**
 * @brief Give one viewer the live data lease, only while logging. See
 * live_data_start().
 *
 * @param token Set to the viewer's token on success.
 * @return APP_CONTROL_OK, CONFLICT if not running, BUSY if another viewer holds
 *         the lease, or FAILED if token is NULL or the lock was held over 1 s.
 */
app_control_result_t app_control_start_live_data(uint32_t *token);
/* Initialized before CAN dispatch starts. Never wait for CAN data while holding this lock. */
void app_control_lock_can_processing(void);
void app_control_unlock_can_processing(void);

/**
 * @brief Fill a status snapshot for /api/status and the serial console.
 *
 * Takes no lock, so fields may straddle a start or stop.
 *
 * @param status Snapshot to fill; NULL does nothing.
 */
void app_control_get_status(app_status_t *status);

/**
 * @brief Log the status for the serial status command.
 */
void app_control_print_status(void);
