#include "logger/data_logger.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "time/absolute_clock.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_QUEUE_LENGTH        256
#define LOG_RECORDS_PER_BLOCK   100
#define LOG_FLUSH_INTERVAL_MS   15000
#define LOG_PATH_LENGTH         64
#define DEFAULT_NAME_GPS_WAIT_MS 3000

static const char *TAG = "DataLogger";
static QueueHandle_t log_queue;
static SemaphoreHandle_t file_mutex;
static FILE *log_file;
static FILE *utc_file;
static char utc_path[LOG_PATH_LENGTH + 8];
typedef struct { can_message_t message; int64_t utc_ms; } queued_sample_t;
static char log_path[LOG_PATH_LENGTH];
static volatile logger_state_t state = LOGGER_IDLE;
static volatile uint32_t queue_drops;

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

data_logger_start_result_t data_logger_start(const char *filename)
{
    if (state != LOGGER_IDLE) {
        ESP_LOGW(TAG, "Logger is already active");
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
    int log_fd = open(candidate_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    log_file = log_fd >= 0 ? fdopen(log_fd, "wb") : NULL;
    if (!log_file && log_fd >= 0) close(log_fd);
    int utc_fd = log_file
               ? open(candidate_utc_path, O_WRONLY | O_CREAT | O_EXCL, 0666)
               : -1;
    bool utc_created = utc_fd >= 0;
    utc_file = utc_fd >= 0 ? fdopen(utc_fd, "wb") : NULL;
    if (!utc_file && utc_fd >= 0) close(utc_fd);
    int open_error = errno;
    if (log_file && !utc_file) {
        fclose(log_file);
        log_file = NULL;
        unlink(candidate_path);
        if (utc_created) unlink(candidate_utc_path);
    }
    if (!log_file) {
        xSemaphoreGive(file_mutex);
        ESP_LOGE(TAG, "Cannot open %s", candidate_path);
        return open_error == EEXIST ? DATA_LOGGER_START_EXISTS
                                    : DATA_LOGGER_START_OPEN_FAILED;
    }
    snprintf(log_path, sizeof(log_path), "%s", candidate_path);
    snprintf(utc_path, sizeof(utc_path), "%s", candidate_utc_path);
    xQueueReset(log_queue);
    state = LOGGER_RUNNING;
    xSemaphoreGive(file_mutex);
    ESP_LOGI(TAG, "Logging started: %s", log_path);
    return DATA_LOGGER_START_OK;
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

    if (state != LOGGER_IDLE &&
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
    state = LOGGER_STOPPING;
    ESP_LOGI(TAG, "Stopping; queued samples will be written before close");
    return true;
}

void data_logger_enqueue(const can_message_t *message)
{
    if (state != LOGGER_RUNNING) return;
    queued_sample_t sample = { .message = *message,
        .utc_ms = absolute_clock_sample_utc((uint32_t)(message->data >> 32)) };
    if (xQueueSend(log_queue, &sample, 0) != pdPASS) {
        queue_drops++;
    }
}

logger_state_t data_logger_state(void) { return state; }
const char *data_logger_path(void) { return log_path[0] ? log_path : "none"; }
uint32_t data_logger_drop_count(void) { return queue_drops; }

const char *data_logger_state_name(void)
{
    return state == LOGGER_RUNNING ? "running" :
           state == LOGGER_STOPPING ? "stopping" : "idle";
}

static bool write_records(const int64_t records[][2], const int64_t *utc, size_t count)
{
    size_t bytes = count * sizeof(records[0]);
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) return false;
    size_t written = log_file ? fwrite(records, 1, bytes, log_file) : 0;
    /* Only append UTC for complete records successfully written to the binary. */
    size_t complete = written / sizeof(records[0]);
    if (utc_file && (fwrite(utc, sizeof(*utc), complete, utc_file) != complete ||
                     fflush(utc_file) != 0)) {
        ESP_LOGE(TAG, "UTC sidecar write failed; later UTC values unavailable");
        fclose(utc_file);
        utc_file = NULL;
    }
    if (written != bytes && utc_file) {
        /* A partial binary record makes later positional metadata unsafe. */
        fclose(utc_file);
        utc_file = NULL;
    }
    if (log_file) fflush(log_file);
    xSemaphoreGive(file_mutex);
    if (written != bytes) {
        ESP_LOGE(TAG, "SD write failed (%u/%u bytes)",
                 (unsigned)written, (unsigned)bytes);
        return false;
    }
    return true;
}

static bool checkpoint_file(void)
{
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (log_file) fclose(log_file);
    log_file = fopen(log_path, "ab");
    if (utc_file) {
        fclose(utc_file);
        utc_file = fopen(utc_path, "ab");
        if (!utc_file) ESP_LOGE(TAG, "Cannot reopen UTC sidecar");
    }
    xSemaphoreGive(file_mutex);
    if (!log_file) {
        ESP_LOGE(TAG, "Checkpoint saved, but cannot reopen %s", log_path);
        return false;
    }
    ESP_LOGI(TAG, "Checkpoint saved; file closed and reopened: %s", log_path);
    return true;
}

static void writer_task(void *argument)
{
    (void)argument;
    int64_t records[LOG_RECORDS_PER_BLOCK][2];
    int64_t utc[LOG_RECORDS_PER_BLOCK];
    size_t count = 0;
    queued_sample_t sample;
    TickType_t last_checkpoint = xTaskGetTickCount();
    while (true) {
        if (xQueueReceive(log_queue, &sample, pdMS_TO_TICKS(100)) == pdTRUE) {
            records[count][0] = sample.message.id;
            records[count][1] = (int64_t)sample.message.data;
            utc[count] = sample.utc_ms;
            if (++count == LOG_RECORDS_PER_BLOCK) {
                (void)write_records(records, utc, count);
                count = 0;
            }
        }
        TickType_t now = xTaskGetTickCount();
        if (state == LOGGER_RUNNING &&
            now - last_checkpoint >= pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS)) {
            if (count == 0 || write_records(records, utc, count)) {
                count = 0;
                (void)checkpoint_file();
            }
            last_checkpoint = now;
        }
        if (state == LOGGER_STOPPING && uxQueueMessagesWaiting(log_queue) == 0) {
            if (count > 0) {
                (void)write_records(records, utc, count);
                count = 0;
            }
            if (xSemaphoreTake(file_mutex, portMAX_DELAY) == pdTRUE) {
                if (log_file) fclose(log_file);
                log_file = NULL;
                if (utc_file) fclose(utc_file);
                utc_file = NULL;
                xSemaphoreGive(file_mutex);
            }
            state = LOGGER_IDLE;
            ESP_LOGI(TAG, "Logging stopped and file closed");
        }
        if (state == LOGGER_IDLE) last_checkpoint = now;
    }
}

esp_err_t data_logger_init(void)
{
    log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(queued_sample_t));
    file_mutex = xSemaphoreCreateMutex();
    if (!log_queue || !file_mutex) return ESP_ERR_NO_MEM;
    return xTaskCreate(writer_task, "sd_writer", 6144, NULL, 10, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
