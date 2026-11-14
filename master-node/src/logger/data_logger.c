#include "logger/data_logger.h"

#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "health/node_health.h"
#include "time/absolute_clock.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_QUEUE_LENGTH        256
#define LOG_RECORDS_PER_BLOCK   100
#define LOG_FLUSH_INTERVAL_MS   1000
/* Nodes may send health events until they hear STOP. */
#define LOG_STOP_GRACE_MS       200
#define LOG_PATH_LENGTH         64
#define DEFAULT_NAME_GPS_WAIT_MS 3000

static const char *TAG = "DataLogger";
static QueueHandle_t log_queue;
static SemaphoreHandle_t file_mutex;
/* Guards health_admission_open and the corresponding queue insertion. */
static SemaphoreHandle_t health_admission_mutex;
static int log_fd = -1;
static int utc_fd = -1;
static uint32_t records_saved;
static char utc_path[LOG_PATH_LENGTH + 8];
typedef struct { can_message_t message; int64_t utc_ms; } queued_sample_t;
static char log_path[LOG_PATH_LENGTH];
static volatile logger_state_t state = LOGGER_IDLE;
static volatile TickType_t stop_tick;
static bool health_admission_open;
static atomic_uint queue_drops;
static atomic_uint write_drops;

bool data_logger_valid_filename(const char *filename)
{
    if (!filename) return false;
    size_t length = strlen(filename);
    if (length < 5 || length > DATA_LOGGER_FILENAME_MAX ||
        strcmp(filename + length - 4, ".bin") != 0) return false;
    for (size_t i = 0; i < length - 4; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

/**
 * @brief Pick a free default log path, named by GPS date and time if available.
 *
 * Waits up to 3 s for GPS time, else uses log_NNNN.bin. A name is taken if its
 * .bin or .utc file exists.
 *
 * @param path Full path under /sdcard.
 * @param path_size Size of the buffer in bytes.
 */
static void choose_default_log_path(char *path, size_t path_size)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t wait_ticks = pdMS_TO_TICKS(DEFAULT_NAME_GPS_WAIT_MS);
    int64_t utc_ms;
    do {
        utc_ms = absolute_clock_now_utc();
        if (utc_ms > 0) break;
        if (xTaskGetTickCount() - started >= wait_ticks) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (true);

    if (utc_ms > 0) {
        time_t seconds = (time_t)(utc_ms / 1000);
        struct tm utc;
        char timestamp[32];
        if (gmtime_r(&seconds, &utc) &&
            strftime(timestamp, sizeof(timestamp),
                     "log_%Y-%m-%d_%H-%M-%S", &utc)) {
            struct stat info;
            for (unsigned suffix = 0; ; suffix++) {
                if (suffix == 0) {
                    snprintf(path, path_size, "/sdcard/%s.bin", timestamp);
                } else {
                    snprintf(path, path_size, "/sdcard/%s_%02u.bin",
                             timestamp, suffix);
                }
                char sidecar[LOG_PATH_LENGTH + 8];
                snprintf(sidecar, sizeof(sidecar), "%s.utc", path);
                if (stat(path, &info) != 0 && stat(sidecar, &info) != 0) return;
            }
        }
    }

    /* Keep the original numbering scheme when GPS time misses the deadline. */
    struct stat info;
    for (unsigned index = 1; ; index++) {
        snprintf(path, path_size, "/sdcard/log_%04u.bin", index);
        char sidecar[LOG_PATH_LENGTH + 8];
        snprintf(sidecar, sizeof(sidecar), "%s.utc", path);
        if (stat(path, &info) != 0 && stat(sidecar, &info) != 0) return;
    }
}

data_logger_start_result_t data_logger_prepare(const char *filename)
{
    if (state != LOGGER_IDLE || log_fd >= 0) {
        ESP_LOGW(TAG, "Logger is already active or prepared");
        return DATA_LOGGER_START_ACTIVE;
    }
    char candidate_path[LOG_PATH_LENGTH];
    char candidate_utc_path[LOG_PATH_LENGTH + 8];
    if (filename) {
        if (!data_logger_valid_filename(filename)) {
            ESP_LOGW(TAG, "Invalid log filename");
            return DATA_LOGGER_START_INVALID_NAME;
        }
        snprintf(candidate_path, sizeof(candidate_path), "/sdcard/%s", filename);
    } else {
        choose_default_log_path(candidate_path, sizeof(candidate_path));
    }
    snprintf(candidate_utc_path, sizeof(candidate_utc_path), "%s.utc",
             candidate_path);
    struct stat info;
    if (stat(candidate_path, &info) == 0 ||
        stat(candidate_utc_path, &info) == 0) {
        ESP_LOGW(TAG, "Log filename already exists: %s", candidate_path);
        return DATA_LOGGER_START_EXISTS;
    }
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) {
        return DATA_LOGGER_START_OPEN_FAILED;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_APPEND;
    log_fd = open(candidate_path, flags, 0666);
    utc_fd = log_fd >= 0 ? open(candidate_utc_path, flags, 0666) : -1;
    int open_error = errno;
    if (log_fd >= 0 && utc_fd < 0) {
        close(log_fd);
        log_fd = -1;
        unlink(candidate_path);
    }
    if (log_fd < 0) {
        xSemaphoreGive(file_mutex);
        ESP_LOGE(TAG, "Cannot open %s", candidate_path);
        return open_error == EEXIST ? DATA_LOGGER_START_EXISTS
                                    : DATA_LOGGER_START_OPEN_FAILED;
    }
    snprintf(log_path, sizeof(log_path), "%s", candidate_path);
    snprintf(utc_path, sizeof(utc_path), "%s", candidate_utc_path);
    records_saved = 0;
    xSemaphoreGive(file_mutex);
    return DATA_LOGGER_START_OK;
}

/* Called by recording control after preparation and before enabling recording. */
void data_logger_clear_queue(void)
{
    configASSERT(state == LOGGER_IDLE);
    xQueueReset(log_queue);
}

void data_logger_start(void)
{
    configASSERT(state == LOGGER_IDLE && log_fd >= 0 && utc_fd >= 0);
    xSemaphoreTake(health_admission_mutex, portMAX_DELAY);
    health_admission_open = true;
    state = LOGGER_RUNNING;
    xSemaphoreGive(health_admission_mutex);
    node_health_set(HEALTH_SD_WRITE, false);
    ESP_LOGI(TAG, "Logging started: %s", log_path);
}

data_logger_rename_result_t data_logger_rename(const char *old_filename,
                                               const char *new_filename)
{
    if (!data_logger_valid_filename(old_filename) ||
        !data_logger_valid_filename(new_filename)) {
        return DATA_LOGGER_RENAME_INVALID_NAME;
    }
    if (strcmp(old_filename, new_filename) == 0) return DATA_LOGGER_RENAME_OK;
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) {
        return DATA_LOGGER_RENAME_FAILED;
    }

    char old_path[LOG_PATH_LENGTH], new_path[LOG_PATH_LENGTH];
    char old_utc_path[LOG_PATH_LENGTH + 8], new_utc_path[LOG_PATH_LENGTH + 8];
    snprintf(old_path, sizeof(old_path), "/sdcard/%s", old_filename);
    snprintf(new_path, sizeof(new_path), "/sdcard/%s", new_filename);
    snprintf(old_utc_path, sizeof(old_utc_path), "%s.utc", old_path);
    snprintf(new_utc_path, sizeof(new_utc_path), "%s.utc", new_path);

    if ((state != LOGGER_IDLE || log_fd >= 0) &&
        (strcmp(old_path, log_path) == 0 || strcmp(new_path, log_path) == 0)) {
        xSemaphoreGive(file_mutex);
        return DATA_LOGGER_RENAME_ACTIVE;
    }
    struct stat info;
    if (stat(old_path, &info) != 0 || !S_ISREG(info.st_mode)) {
        xSemaphoreGive(file_mutex);
        return DATA_LOGGER_RENAME_NOT_FOUND;
    }
    if (stat(new_path, &info) == 0 || stat(new_utc_path, &info) == 0) {
        xSemaphoreGive(file_mutex);
        return DATA_LOGGER_RENAME_EXISTS;
    }

    bool has_utc = stat(old_utc_path, &info) == 0 && S_ISREG(info.st_mode);
    if (rename(old_path, new_path) != 0) {
        xSemaphoreGive(file_mutex);
        return DATA_LOGGER_RENAME_FAILED;
    }
    if (has_utc && rename(old_utc_path, new_utc_path) != 0) {
        if (rename(new_path, old_path) != 0) {
            ESP_LOGE(TAG, "Failed to roll back partial rename to %s", old_path);
        }
        xSemaphoreGive(file_mutex);
        return DATA_LOGGER_RENAME_FAILED;
    }
    if (strcmp(log_path, old_path) == 0) {
        snprintf(log_path, sizeof(log_path), "%s", new_path);
        snprintf(utc_path, sizeof(utc_path), "%s", new_utc_path);
    }
    xSemaphoreGive(file_mutex);
    ESP_LOGI(TAG, "Renamed %s to %s", old_filename, new_filename);
    return DATA_LOGGER_RENAME_OK;
}

bool data_logger_stop(void)
{
    if (state != LOGGER_RUNNING) {
        ESP_LOGW(TAG, "Logger is not running");
        return false;
    }
    stop_tick = xTaskGetTickCount();
    state = LOGGER_STOPPING;
    ESP_LOGI(TAG, "Stopping; queued samples will be written before close");
    return true;
}

/**
 * @brief Queue a frame with its UTC time for writer_task.
 *
 * A full queue drops the frame and counts HEALTH_SAMPLES_DROPPED.
 *
 * @param message Frame to log.
 * @return true if queued.
 */
static bool enqueue(const can_message_t *message)
{
    queued_sample_t sample = { .message = *message,
        .utc_ms = absolute_clock_sample_utc((uint32_t)(message->data >> 32)) };
    if (xQueueSend(log_queue, &sample, 0) != pdPASS) {
        atomic_fetch_add(&queue_drops, 1);
        node_health_count(HEALTH_SAMPLES_DROPPED, 1, message->id, ESP_OK);
        return false;
    }
    return true;
}

void data_logger_enqueue(const can_message_t *message)
{
    if (state != LOGGER_RUNNING) return;
    (void)enqueue(message);
}

bool data_logger_enqueue_health(const can_message_t *message)
{
    bool queued = false;
    if (xSemaphoreTake(health_admission_mutex, portMAX_DELAY) == pdTRUE) {
        if (health_admission_open) queued = enqueue(message);
        xSemaphoreGive(health_admission_mutex);
    }
    return queued;
}

logger_state_t data_logger_state(void) { return state; }
const char *data_logger_path(void) { return log_path[0] ? log_path : "none"; }
uint32_t data_logger_drop_count(void)
{
    return atomic_load(&queue_drops) + atomic_load(&write_drops);
}

const char *data_logger_state_name(void)
{
    return state == LOGGER_RUNNING ? "running" :
           state == LOGGER_STOPPING ? "stopping" : "idle";
}

/** @brief Close both log files. Call with file_mutex held. */
static void close_files(void)
{
    if (log_fd >= 0) close(log_fd);
    if (utc_fd >= 0) close(utc_fd);
    log_fd = -1;
    utc_fd = -1;
}

/**
 * @brief Reopen both log files and cut them back to the saved records.
 *
 * Call with file_mutex held.
 *
 * @return false if either file could not be reopened or cut; both are then closed.
 */
static bool reopen_files(void)
{
    close_files();
    log_fd = open(log_path, O_WRONLY | O_APPEND);
    utc_fd = log_fd >= 0 ? open(utc_path, O_WRONLY | O_APPEND) : -1;
    /* Commit the .bin cut before the .utc grows again, or a power cut can pair cut
     * .bin records with newer UTC entries. */
    if (log_fd >= 0 && utc_fd >= 0 &&
        ftruncate(log_fd, (off_t)records_saved * 16) == 0 && fsync(log_fd) == 0 &&
        ftruncate(utc_fd, (off_t)records_saved * 8) == 0) {
        return true;
    }
    close_files();
    return false;
}

/**
 * @brief Append and sync one block to the .utc, then to the .bin, so every saved
 * .bin record has its UTC entry.
 *
 * Call with file_mutex held.
 *
 * @return false on any short write or failed sync.
 */
static bool write_block(const int64_t records[][2], const int64_t *utc, size_t count)
{
    ssize_t utc_bytes = (ssize_t)(count * sizeof(*utc));
    ssize_t bin_bytes = (ssize_t)(count * sizeof(records[0]));
    return write(utc_fd, utc, utc_bytes) == utc_bytes && fsync(utc_fd) == 0 &&
           write(log_fd, records, bin_bytes) == bin_bytes && fsync(log_fd) == 0;
}

/**
 * @brief Save one block, retrying once after reopening the files.
 *
 * A block that still fails is cut off both files and counted as dropped. A failure
 * sets HEALTH_SD_WRITE until a block is saved again.
 */
static void save_records(const int64_t records[][2], const int64_t *utc, size_t count)
{
    xSemaphoreTake(file_mutex, portMAX_DELAY);
    bool saved = write_block(records, utc, count);
    if (!saved) {
        /* FatFs refuses every later write to a file after one error until it is reopened. */
        ESP_LOGW(TAG, "SD write failed; reopening %s", log_path);
        node_health_set(HEALTH_SD_WRITE, true);
        saved = reopen_files() && write_block(records, utc, count);
    }
    if (saved) {
        records_saved += count;
    } else {
        ESP_LOGE(TAG, "SD write failed; %u records lost", (unsigned)count);
        atomic_fetch_add(&write_drops, count);
        for (size_t i = 0; i < count; i++) {
            node_health_count(HEALTH_SAMPLES_DROPPED, 1, (uint32_t)records[i][0], ESP_FAIL);
        }
        (void)reopen_files();
    }
    node_health_set(HEALTH_SD_WRITE, !saved);
    xSemaphoreGive(file_mutex);
}

/**
 * @brief Task that writes queued frames to the SD card.
 *
 * Writes 100-record blocks, and any partial block every 1 s. After stop, once
 * the queue is empty and 200 ms have passed, it writes the rest, closes both
 * files and sets LOGGER_IDLE.
 */
static void writer_task(void *argument)
{
    (void)argument;
    int64_t records[LOG_RECORDS_PER_BLOCK][2];
    int64_t utc[LOG_RECORDS_PER_BLOCK];
    size_t count = 0;
    queued_sample_t sample;
    TickType_t last_flush = xTaskGetTickCount();
    while (true) {
        if (state == LOGGER_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            last_flush = xTaskGetTickCount();
            continue;
        }
        if (xQueueReceive(log_queue, &sample, pdMS_TO_TICKS(100)) == pdTRUE) {
            records[count][0] = sample.message.id;
            records[count][1] = (int64_t)sample.message.data;
            utc[count] = sample.utc_ms;
            if (++count == LOG_RECORDS_PER_BLOCK) {
                save_records(records, utc, count);
                count = 0;
            }
        }
        TickType_t now = xTaskGetTickCount();
        if (state == LOGGER_RUNNING &&
            now - last_flush >= pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS)) {
            if (count > 0) save_records(records, utc, count);
            count = 0;
            last_flush = now;
        }
        bool close_log = false;
        if (state == LOGGER_STOPPING &&
            (int32_t)(now - stop_tick) >= (int32_t)pdMS_TO_TICKS(LOG_STOP_GRACE_MS) &&
            xSemaphoreTake(health_admission_mutex, portMAX_DELAY) == pdTRUE) {
            if (state == LOGGER_STOPPING) {
                health_admission_open = false;
                close_log = uxQueueMessagesWaiting(log_queue) == 0;
            }
            xSemaphoreGive(health_admission_mutex);
        }
        if (close_log) {
            if (count > 0) save_records(records, utc, count);
            count = 0;
            xSemaphoreTake(file_mutex, portMAX_DELAY);
            close_files();
            xSemaphoreGive(file_mutex);
            state = LOGGER_IDLE;
            node_health_set(HEALTH_SD_WRITE, false);
            ESP_LOGI(TAG, "Logging stopped and file closed");
        }
    }
}

esp_err_t data_logger_init(void)
{
    log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(queued_sample_t));
    file_mutex = xSemaphoreCreateMutex();
    health_admission_mutex = xSemaphoreCreateMutex();
    if (!log_queue || !file_mutex || !health_admission_mutex) return ESP_ERR_NO_MEM;
    return xTaskCreate(writer_task, "sd_writer", 6144, NULL, 10, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
