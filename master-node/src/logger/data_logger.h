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

/**
 * @brief Create the sample queue and start the SD writer task.
 *
 * Files are saved every 15 s, so a power cut loses at most the last 15 s.
 *
 * @retval ESP_OK          The writer is running, idle until data_logger_start().
 * @retval ESP_ERR_NO_MEM  Creating the queue, mutex or task failed.
 */
esp_err_t data_logger_init(void);

/**
 * @brief Open a new log file and its .utc side file, and start logging.
 *
 * With no name, it waits up to 3 s for GPS time to name the file by date, else
 * uses log_NNNN.bin. Clears samples left in the queue.
 *
 * @param filename Name on the SD card, or NULL for a default name.
 * @return DATA_LOGGER_START_OK, or why it did not start.
 */
data_logger_start_result_t data_logger_start(const char *filename);

/**
 * @brief Ask the writer to finish and close the log.
 *
 * Returns at once. The state stays LOGGER_STOPPING until the queue is saved and
 * both files are closed, at least 200 ms.
 *
 * @return true if logging was running.
 */
bool data_logger_stop(void);

/**
 * @brief Check whether a log file name is allowed.
 *
 * Letters, digits, _ and -, then .bin, at most DATA_LOGGER_FILENAME_MAX
 * characters. No folders.
 *
 * @param filename Name to check; NULL is invalid.
 * @return true if allowed.
 */
bool data_logger_valid_filename(const char *filename);

/**
 * @brief Rename a log file and its .utc side file.
 *
 * Renaming a file to its own name succeeds and does nothing.
 *
 * @param old_filename Current name, without /sdcard/.
 * @param new_filename New name, without /sdcard/.
 * @return DATA_LOGGER_RENAME_OK, or why it failed.
 */
data_logger_rename_result_t data_logger_rename(const char *old_filename,
                                               const char *new_filename);

/**
 * @brief Queue a sensor frame for the log file; ignored unless running.
 *
 * See enqueue() in data_logger.c.
 *
 * @param message Received sensor frame.
 */
void data_logger_enqueue(const can_message_t *message);

/**
 * @brief Queue a health frame like data_logger_enqueue(), but also while
 * stopping.
 *
 * Accepted while running and during the 200 ms stop grace period. The writer
 * closes admission before its final queue drain.
 *
 * @param message Received health frame.
 * @return false if the frame was not queued.
 */
bool data_logger_enqueue_health(const can_message_t *message);

/**
 * @brief Get the logger state.
 *
 * @return LOGGER_IDLE, LOGGER_RUNNING or LOGGER_STOPPING.
 */
logger_state_t data_logger_state(void);

/**
 * @brief Get the logger state as text.
 *
 * @return "idle", "running" or "stopping".
 */
const char *data_logger_state_name(void);

/**
 * @brief Get the full path of the current or last log file.
 *
 * Kept after stop and updated on rename.
 *
 * @return For example "/sdcard/log_0001.bin", or "none" before the first run.
 */
const char *data_logger_path(void);

/**
 * @brief Count frames dropped because the log queue was full.
 *
 * @return Frames dropped since boot.
 */
uint32_t data_logger_drop_count(void);
