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
 * The writer saves samples in blocks of 100 records. Every 15 s it closes and
 * reopens the files, so a power cut loses at most the last 15 s.
 *
 * @retval ESP_OK          The writer is running, idle until data_logger_start().
 * @retval ESP_ERR_NO_MEM  The queue, mutex or task could not be created.
 */
esp_err_t data_logger_init(void);

/**
 * @brief Open a new log file and its .utc side file, and start logging.
 *
 * With no name, it waits up to 3 s for GPS time and picks
 * log_YYYY-MM-DD_HH-MM-SS.bin, adding _01, _02, ... if taken; without GPS
 * time it picks the next free log_NNNN.bin. Clears samples left in the queue.
 *
 * @param filename Name on the SD card, or NULL for a default name.
 * @return DATA_LOGGER_START_OK, or why it did not start: already logging
 *         (ACTIVE), bad name (INVALID_NAME), the .bin or .utc file exists
 *         (EXISTS), or a file could not be created (OPEN_FAILED).
 */
data_logger_start_result_t data_logger_start(const char *filename);

/**
 * @brief Ask the writer to finish the log and close it.
 *
 * Returns at once; the state is LOGGER_STOPPING until the writer has saved
 * every queued sample, waited 200 ms for late health events, and closed both
 * files. Then it is LOGGER_IDLE.
 *
 * @return true if logging was running, false otherwise.
 */
bool data_logger_stop(void);

/**
 * @brief Check whether a log file name is allowed.
 *
 * Allowed: letters, digits, _ and -, then .bin, at most
 * DATA_LOGGER_FILENAME_MAX characters in total. No folders.
 *
 * @param filename Name to check; NULL is invalid.
 * @return true if the name is allowed.
 */
bool data_logger_valid_filename(const char *filename);

/**
 * @brief Rename a log file on the SD card, with its .utc side file.
 *
 * If the side file cannot be renamed, the .bin is renamed back. Renaming a
 * file to its own name succeeds and does nothing.
 *
 * @param old_filename Current name, without /sdcard/.
 * @param new_filename New name, without /sdcard/.
 * @return DATA_LOGGER_RENAME_OK, or why it failed: a bad name (INVALID_NAME),
 *         either name is the log being written (ACTIVE), no such file
 *         (NOT_FOUND), the new .bin or .utc already exists (EXISTS), or the
 *         SD card refused (FAILED).
 */
data_logger_rename_result_t data_logger_rename(const char *old_filename,
                                               const char *new_filename);

/**
 * @brief Queue a sensor frame for the log file.
 *
 * Only while running; see enqueue() in data_logger.c.
 *
 * @param message Received sensor frame.
 * @return false if the sample was not queued: not running, or queue full.
 */
bool data_logger_enqueue(const can_message_t *message);

/**
 * @brief Queue a health frame for the log file, like data_logger_enqueue().
 *
 * Also accepts for 200 ms after stop: a node sends events until it hears
 * STOP and counts a sent event as delivered. See enqueue() in data_logger.c.
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
 * @brief Get the logger state as text for status output.
 *
 * @return "idle", "running" or "stopping".
 */
const char *data_logger_state_name(void);

/**
 * @brief Get the full path of the current or last log file.
 *
 * Kept after stop, and updated if that file is renamed.
 *
 * @return For example "/sdcard/log_0001.bin", or "none" before the first run.
 */
const char *data_logger_path(void);

/**
 * @brief Count frames dropped because the log queue was full.
 *
 * @return Frames dropped since boot; never reset.
 */
uint32_t data_logger_drop_count(void);
