#include "logger/data_logger.h"

#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_QUEUE_LENGTH        256
#define LOG_RECORDS_PER_BLOCK   100
#define LOG_PATH_LENGTH         64

static const char *TAG = "DataLogger";
static QueueHandle_t log_queue;
static SemaphoreHandle_t file_mutex;
static FILE *log_file;
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
    if (state == LOGGER_RUNNING &&
        xQueueSend(log_queue, message, 0) != pdPASS) {
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

static bool write_records(const int64_t records[][2], size_t count)
{
    size_t bytes = count * sizeof(records[0]);
    if (xSemaphoreTake(file_mutex, portMAX_DELAY) != pdTRUE) return false;
    size_t written = log_file ? fwrite(records, 1, bytes, log_file) : 0;
    if (log_file) fflush(log_file);
    xSemaphoreGive(file_mutex);
    if (written != bytes) {
        ESP_LOGE(TAG, "SD write failed (%u/%u bytes)",
                 (unsigned)written, (unsigned)bytes);
        return false;
    }
    return true;
}

static void writer_task(void *argument)
{
    (void)argument;
    int64_t records[LOG_RECORDS_PER_BLOCK][2];
    size_t count = 0;
    can_message_t message;
    while (true) {
        if (xQueueReceive(log_queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            records[count][0] = message.id;
            records[count][1] = (int64_t)message.data;
            if (++count == LOG_RECORDS_PER_BLOCK) {
                (void)write_records(records, count);
                count = 0;
            }
        }
        if (state == LOGGER_STOPPING && uxQueueMessagesWaiting(log_queue) == 0) {
            if (count > 0) {
                (void)write_records(records, count);
                count = 0;
            }
            if (xSemaphoreTake(file_mutex, portMAX_DELAY) == pdTRUE) {
                if (log_file) fclose(log_file);
                log_file = NULL;
                xSemaphoreGive(file_mutex);
            }
            state = LOGGER_IDLE;
            ESP_LOGI(TAG, "Logging stopped and file closed");
        }
    }
}

esp_err_t data_logger_init(void)
{
    log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(can_message_t));
    file_mutex = xSemaphoreCreateMutex();
    if (!log_queue || !file_mutex) return ESP_ERR_NO_MEM;
    return xTaskCreate(writer_task, "sd_writer", 6144, NULL, 10, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
