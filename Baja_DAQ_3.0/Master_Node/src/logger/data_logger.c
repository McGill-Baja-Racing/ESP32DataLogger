#include "logger/data_logger.h"

#include <stdio.h>
#include <sys/stat.h>
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

static void choose_log_path(void)
{
    struct stat info;
    for (unsigned index = 1; ; index++) {
        snprintf(log_path, sizeof(log_path), "/sdcard/log_%04u.bin", index);
        if (stat(log_path, &info) != 0) return;
    }
}

bool data_logger_start(void)
{
    if (state != LOGGER_IDLE) {
        ESP_LOGW(TAG, "Logger is already active");
        return false;
    }
    choose_log_path();
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) return false;
    log_file = fopen(log_path, "wb");
    snprintf(utc_path, sizeof(utc_path), "%s.utc", log_path);
    utc_file = log_file ? fopen(utc_path, "wb") : NULL;
    if (log_file && !utc_file) { fclose(log_file); log_file = NULL; }
    xSemaphoreGive(file_mutex);
    if (!log_file) {
        ESP_LOGE(TAG, "Cannot open %s", log_path);
        return false;
    }
    xQueueReset(log_queue);
    state = LOGGER_RUNNING;
    ESP_LOGI(TAG, "Logging started: %s", log_path);
    return true;
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
