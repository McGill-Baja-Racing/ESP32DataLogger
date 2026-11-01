#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "logger/data_logger.h"

typedef enum {
    APP_CONTROL_OK,
    APP_CONTROL_CONFLICT,
    APP_CONTROL_BUSY,
    APP_CONTROL_INVALID_FILENAME,
    APP_CONTROL_FILENAME_EXISTS,
    APP_CONTROL_NODE_PROTOCOL,
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
    const char *node_1;
    const char *node_3;
    const char *node_4;
    const char *node_5;
    const char *node_6;
} app_status_t;

/**
 * @brief Create the mutex that keeps start, stop and live data from racing.
 *
 * Call after firmware_update_init(). If firmware_update_resume_pending(), the
 * firmware is claimed from here, so nothing can start logging before
 * app_control_resume_firmware_update() has updated the nodes.
 *
 * @retval ESP_OK          Ready.
 * @retval ESP_ERR_NO_MEM  The mutex could not be created.
 */
esp_err_t app_control_init(void);

/**
 * @brief Open a new log file, then tell the nodes to start sampling.
 *
 * The file opens first so no sample is lost. A failed START broadcast is
 * logged but still returns APP_CONTROL_OK: nodes also start from the
 * recording bit in the time beacon.
 *
 * @param filename Name on the SD card, or NULL for a default name; see
 *                 data_logger_start().
 * @return APP_CONTROL_OK, or why it did not start: not idle (CONFLICT), the
 *         firmware is claimed or the running image is on trial (BUSY), an expected node runs another protocol
 *         (NODE_PROTOCOL), bad name (INVALID_FILENAME), the file exists
 *         (FILENAME_EXISTS), or the file could not be created or another
 *         start or stop held the lock for over 1 s (FAILED).
 */
app_control_result_t app_control_start_logging(const char *filename);

/**
 * @brief End live data, ask the logger to stop, then tell the nodes to stop.
 *
 * Returns once stopping has begun; the logger is LOGGER_STOPPING until it
 * has saved the queue. A failed STOP broadcast is logged but still returns
 * APP_CONTROL_OK.
 *
 * @return APP_CONTROL_OK, CONFLICT if not running, or FAILED if the lock
 *         was held for over 1 s or the logger refused.
 */
app_control_result_t app_control_stop_logging(void);

/**
 * @brief Claim the firmware files and flash for one upload, delete or update.
 *
 * Until app_control_release_firmware(), logging cannot start and no other
 * claim is given.
 *
 * @return APP_CONTROL_OK, CONFLICT unless the logger is idle, BUSY if already
 *         claimed, or FAILED if the lock was held for over 1 s.
 */
app_control_result_t app_control_claim_firmware(void);

/**
 * @brief Release the claim taken by app_control_claim_firmware().
 */
void app_control_release_firmware(void);

/**
 * @brief Claim the firmware, then start a task that runs
 *        firmware_update_run() and releases the claim.
 *
 * @return APP_CONTROL_OK once the task runs, the app_control_claim_firmware()
 *         result, or FAILED if the task could not start.
 */
app_control_result_t app_control_update_firmware(void);

/**
 * @brief Start the update task on the claim app_control_init() took, if
 *        firmware_update_resume_pending().
 *
 * Call after firmware_update_confirm().
 *
 * @retval ESP_OK          Nothing to resume, or the task is running.
 * @retval ESP_ERR_NO_MEM  The task could not start; the claim is released.
 */
esp_err_t app_control_resume_firmware_update(void);

/**
 * @brief Give one viewer the live data lease, only while logging.
 *
 * See live_data_start().
 *
 * @param token Set to the viewer's token on success.
 * @return APP_CONTROL_OK, CONFLICT if not running, BUSY if another viewer
 *         holds the lease, or FAILED if token is NULL or the lock was held
 *         for over 1 s.
 */
app_control_result_t app_control_start_live_data(uint32_t *token);

/**
 * @brief Fill a status snapshot for /api/status and the serial console.
 *
 * Does not take the lock, so fields may come from either side of a start
 * or stop. Strings are static and stay valid.
 *
 * @param status Snapshot to fill; NULL does nothing.
 */
void app_control_get_status(app_status_t *status);

/**
 * @brief Log the status for the serial status command.
 */
void app_control_print_status(void);
