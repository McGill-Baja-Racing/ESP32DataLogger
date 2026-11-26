#include "app/modules_internal.h"
#include <mutex>

QueueHandle_t log_record_queue;
FILE *logFile = NULL;
int writeCounter = 0;
std::atomic<bool> stop_logging{true};
static std::atomic<bool> session_open{false};
static std::atomic<uint32_t> dropped_records{0};
static std::mutex admission_mutex;
static std::atomic<bool> start_requested{false};
static std::atomic<bool> start_cancelled{false};
static esp_err_t open_session();
SemaphoreHandle_t log_file_mutex = NULL;
uint32_t logBuffer[SAMPLES_PER_BLOCK][SAMPLE_SIZE];

int32_t rpm_to_centi(float rpm)
{
    return (int32_t)lroundf(rpm * 100.0f);
}

bool enqueue_log_record(const log_record_t *record)
{
    if (stop_logging) {
        return false;
    }

    if (log_record_queue == NULL) {
        return false;
    }

    // Never wait behind SD I/O on the controller or CAN dispatch task.
    std::unique_lock<std::mutex> admission(admission_mutex, std::try_to_lock);
    if (!admission.owns_lock()) {
        dropped_records.fetch_add(1);
        return false;
    }
    if (stop_logging || !session_open.load()) {
        return false;
    }
    const BaseType_t queued = xQueueSend(log_record_queue, record, 0);
    if (queued != pdPASS) {
        dropped_records.fetch_add(1);
        ESP_LOGW(TAG, "Log queue FULL - record dropped");
        return false;
    }

    return true;
}

void queue_servo_feedback_log(uint32_t record_type, int32_t primary_value)
{
    log_record_t record = {};
    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = record_type;
    record.primary_value = primary_value;
    (void)enqueue_log_record(&record);
}

void queue_sensor_snapshot_log(uint32_t record_type)
{
    log_record_t record = {};

    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = record_type;
    record.throttle_pct = s_latest_throttle;
    record.engine_rpm_centi = rpm_to_centi(s_latest_engine_rpm);
    record.wheel_rpm_centi = rpm_to_centi(s_latest_wheel_rpm);
    record.target_engine_rpm_centi = rpm_to_centi(s_target_engine_rpm_running);
    record.brake_state = s_latest_brake_state ? 1U : 0U;

    switch (record_type) {
    case LOG_RECORD_TYPE_SENSOR_THROTTLE:
        record.primary_value = (int32_t)s_latest_throttle;
        break;
    case LOG_RECORD_TYPE_SENSOR_ENGINE_RPM:
        record.primary_value = rpm_to_centi(s_latest_engine_rpm);
        break;
    case LOG_RECORD_TYPE_SENSOR_WHEEL_RPM:
        record.primary_value = rpm_to_centi(s_latest_wheel_rpm);
        break;
    case LOG_RECORD_TYPE_SENSOR_BRAKE_STATE:
        record.primary_value = s_latest_brake_state ? 1 : 0;
        break;
    default:
        break;
    }

    (void)enqueue_log_record(&record);
}

void queue_controller_command_log(float command_position_revs,
                                         float command_speed_revs_per_sec,
                                         uint32_t throttle_pct,
                                         float engine_rpm,
                                         float wheel_rpm,
                                         float target_engine_rpm,
                                         bool brake_state)
{
    log_record_t record = {};

    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = LOG_RECORD_TYPE_CONTROLLER_COMMAND;
    record.command_position_counts = (int32_t)lroundf(command_position_revs * 10000.0f);
    record.command_speed_counts =
        (int32_t)lroundf(command_speed_revs_per_sec * 10000.0f / 60.0f);
    record.throttle_pct = throttle_pct;
    record.engine_rpm_centi = rpm_to_centi(engine_rpm);
    record.wheel_rpm_centi = rpm_to_centi(wheel_rpm);
    record.target_engine_rpm_centi = rpm_to_centi(target_engine_rpm);
    record.brake_state = brake_state ? 1U : 0U;

    (void)enqueue_log_record(&record);
}

void close_log_file_if_open(void)
{
    if (logFile) {
        fflush(logFile);
        fclose(logFile);
        logFile = NULL;
        ESP_LOGI(TAG, "Log file closed.");
    }
}

bool reopen_log_file_append(const char *path)
{
    FILE *reopened = fopen(path, "ab");
    if (!reopened) {
        ESP_LOGE(TAG, "Failed to reopen log file: %s", path);
        return false;
    }

    logFile = reopened;
    ESP_LOGI(TAG, "Log file reopened.");
    return true;
}

void write_log_block(const uint32_t (*buf)[SAMPLE_SIZE], size_t sample_count)
{
    const size_t bytes = sample_count * SAMPLE_SIZE * sizeof(uint32_t);
    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        return;
    }

    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Log file is not open");
        return;
    }

    size_t written = fwrite(buf, 1, bytes, logFile);
    fflush(logFile);
    xSemaphoreGive(log_file_mutex);

    if (written != bytes) {
        dropped_records.fetch_add(sample_count);
        stop_logging = true;
        ESP_LOGE(TAG, "Short write! wrote=%u expected=%u",
                 (unsigned)written, (unsigned)bytes);
        return;
    }

    writeCounter++;
    ESP_LOGI(TAG, "Wrote block %d (%u bytes, %u samples)",
             writeCounter, (unsigned)bytes, (unsigned)sample_count);
}

void save_task(void *arg)
{
    const char *path = (const char *)arg;
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(15000));

        if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to lock log file mutex");
            continue;
        }

        if (logFile) {
            ESP_LOGI(TAG, "File SAVED");
            close_log_file_if_open();
            if (!reopen_log_file_append(path)) {
                stop_logging = true;
            }
        } else if (stop_logging) {
            xSemaphoreGive(log_file_mutex);
            ESP_LOGI(TAG, "save_task done.");
            vTaskDelete(NULL);
            return;
        }

        xSemaphoreGive(log_file_mutex);
    }
}

void sd_writer_task(void *arg)
{
    (void)arg;
    size_t sampleIndex = 0;

    while (1) {
        if (start_requested.load()) {
            esp_err_t result = start_cancelled.load() ? ESP_ERR_INVALID_STATE : open_session();
            ESP_LOGI(TAG, "Recording start result: %s", esp_err_to_name(result));
            start_requested.store(false);
        }
        log_record_t msg;
        BaseType_t got_sample = xQueueReceive(log_record_queue, &msg, pdMS_TO_TICKS(100));

        if (got_sample == pdTRUE) {
            if (session_open.load()) {
                logBuffer[sampleIndex][0] = msg.t_us;
                logBuffer[sampleIndex][1] = msg.record_type;
                logBuffer[sampleIndex][2] = (uint32_t)msg.primary_value;
                logBuffer[sampleIndex][3] = (uint32_t)msg.secondary_value;
                logBuffer[sampleIndex][4] = (uint32_t)msg.command_position_counts;
                logBuffer[sampleIndex][5] = (uint32_t)msg.command_speed_counts;
                logBuffer[sampleIndex][6] = msg.throttle_pct;
                logBuffer[sampleIndex][7] = (uint32_t)msg.engine_rpm_centi;
                logBuffer[sampleIndex][8] = (uint32_t)msg.wheel_rpm_centi;
                logBuffer[sampleIndex][9] = (uint32_t)msg.target_engine_rpm_centi;
                logBuffer[sampleIndex][10] = msg.brake_state;
                sampleIndex++;

                if (sampleIndex >= SAMPLES_PER_BLOCK) {
                    write_log_block(logBuffer, sampleIndex);
                    sampleIndex = 0;
                }
            }
        }

        if (stop_logging && session_open.load() && uxQueueMessagesWaiting(log_record_queue) == 0) {
            if (sampleIndex > 0) {
                ESP_LOGI(TAG, "Flushing partial block (%u samples)...", (unsigned)sampleIndex);
                write_log_block(logBuffer, sampleIndex);
            }

            std::lock_guard<std::mutex> admission(admission_mutex);
            (void)xSemaphoreTake(log_file_mutex, portMAX_DELAY);
            if (stop_logging && session_open.load() && uxQueueMessagesWaiting(log_record_queue) == 0) {
                close_log_file_if_open();
                sampleIndex = 0;
                session_open.store(false);
                ESP_LOGI(TAG, "Recording closed; ready for another session.");
            }
            xSemaphoreGive(log_file_mutex);
        }
    }
}

void create_unique_log_filename(char *out_path, size_t max_len)
{
    int index = 1;
    struct stat st;
    while (1) {
        snprintf(out_path, max_len, "/sdcard/log_%04d.bin", index);
        if (stat(out_path, &st) != 0) return; // doesn't exist => ok
        index++;
    }
}


static esp_err_t open_session()
{
    std::lock_guard<std::mutex> admission(admission_mutex);
    if (xSemaphoreTake(log_file_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (session_open.load()) { xSemaphoreGive(log_file_mutex); return ESP_ERR_INVALID_STATE; }
    char path[64];
    create_unique_log_filename(path, sizeof(path));
    logFile = fopen(path, "wb");
    if (!logFile) { xSemaphoreGive(log_file_mutex); return ESP_FAIL; }
    writeCounter = 0;
    stop_logging = false;
    if (start_cancelled.load()) {
        stop_logging = true;
        close_log_file_if_open();
        xSemaphoreGive(log_file_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    session_open.store(true);
    xSemaphoreGive(log_file_mutex);
    ESP_LOGI(TAG, "Recording started: %s", path);
    return ESP_OK;
}

esp_err_t logger_session_start()
{
    // Called only by the console. All filesystem work belongs to the writer.
    if (session_open.load() || start_requested.load()) return ESP_ERR_INVALID_STATE;
    start_cancelled.store(false);
    start_requested.store(true);
    return ESP_OK;
}

void logger_session_stop()
{
    start_cancelled.store(true);
    stop_logging = true;
}

uint32_t logger_dropped_records() { return dropped_records.load(); }
