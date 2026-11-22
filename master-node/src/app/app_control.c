#include "app/app_control.h"

#include <inttypes.h>

#include "can/can_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "node_state/node_registry.h"
#include "live/live_data.h"
#include "time/time_beacon.h"

static const char *TAG = "AppControl";
static SemaphoreHandle_t lifecycle_mutex;

esp_err_t app_control_init(void)
{
    lifecycle_mutex = xSemaphoreCreateMutex();
    return lifecycle_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

app_control_result_t app_control_start_logging(const char *filename)
{
    if (!lifecycle_mutex ||
        xSemaphoreTake(lifecycle_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return APP_CONTROL_FAILED;
    }
    if (data_logger_state() != LOGGER_IDLE) {
        ESP_LOGW(TAG, "Start rejected while logger is %s",
                 data_logger_state_name());
        xSemaphoreGive(lifecycle_mutex);
        return APP_CONTROL_CONFLICT;
    }
    data_logger_start_result_t start_result = data_logger_prepare(filename);
    if (start_result != DATA_LOGGER_START_OK) {
        xSemaphoreGive(lifecycle_mutex);
        if (start_result == DATA_LOGGER_START_INVALID_NAME) {
            return APP_CONTROL_INVALID_FILENAME;
        }
        if (start_result == DATA_LOGGER_START_EXISTS) {
            return APP_CONTROL_FILENAME_EXISTS;
        }
        if (start_result == DATA_LOGGER_START_ACTIVE) {
            return APP_CONTROL_CONFLICT;
        }
        return APP_CONTROL_FAILED;
    }
    /* Files are ready. Clear both queues under the recording lock before we restart recording. */
    data_logger_clear_queue();
    can_master_clear_receive_queue();
    data_logger_start();
    node_registry_set_monitoring(true);
    time_beacon_set_recording(true);
    esp_err_t error = can_master_start_nodes();
    xSemaphoreGive(lifecycle_mutex);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Logger started, but START broadcast failed: %s",
                 esp_err_to_name(error));
    }
    return APP_CONTROL_OK;
}

app_control_result_t app_control_stop_logging(void)
{
    if (!lifecycle_mutex ||
        xSemaphoreTake(lifecycle_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return APP_CONTROL_FAILED;
    }
    if (data_logger_state() != LOGGER_RUNNING) {
        ESP_LOGW(TAG, "Stop rejected while logger is %s",
                 data_logger_state_name());
        xSemaphoreGive(lifecycle_mutex);
        return APP_CONTROL_CONFLICT;
    }
    live_data_force_stop();
    if (!data_logger_stop()) {
        xSemaphoreGive(lifecycle_mutex);
        return APP_CONTROL_FAILED;
    }
    time_beacon_set_recording(false);
    node_registry_set_monitoring(false);
    esp_err_t error = can_master_stop_nodes();
    xSemaphoreGive(lifecycle_mutex);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Logger stopping, but STOP broadcast failed: %s",
                 esp_err_to_name(error));
    }
    return APP_CONTROL_OK;
}

app_control_result_t app_control_start_live_data(uint32_t *token)
{
    if (!lifecycle_mutex || !token ||
        xSemaphoreTake(lifecycle_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return APP_CONTROL_FAILED;
    }
    if (data_logger_state() != LOGGER_RUNNING) {
        xSemaphoreGive(lifecycle_mutex);
        return APP_CONTROL_CONFLICT;
    }
    live_data_result_t result = live_data_start(token);
    xSemaphoreGive(lifecycle_mutex);
    if (result == LIVE_DATA_BUSY) return APP_CONTROL_BUSY;
    return result == LIVE_DATA_OK ? APP_CONTROL_OK : APP_CONTROL_FAILED;
}

/* CAN dispatch holds this same lock from dequeue through handler completion. */
void app_control_lock_can_processing(void)
{
    (void)xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
}

void app_control_unlock_can_processing(void)
{
    xSemaphoreGive(lifecycle_mutex);
}

/* Collect logger, CAN, and source health for both web and serial status. */
void app_control_get_status(app_status_t *status)
{
    if (!status) return;
    status->logger_state = data_logger_state();
    status->current_file = data_logger_path();
    status->can_drops = can_master_rx_drop_count();
    status->can_errors_available = can_master_error_counts(
        &status->can_rx_errors, &status->can_tx_errors) == ESP_OK;
    status->log_drops = data_logger_drop_count();
    status->live_enabled = live_data_is_enabled();
    aim_adapter_status(&status->aim);
    status->node_1 = node_registry_state_name(1);
    status->node_3 = node_registry_state_name(3);
    status->node_4 = node_registry_state_name(4);
    status->node_5 = node_registry_state_name(5);
    status->node_6 = node_registry_state_name(6);
    status->node_7 = node_registry_state_name(7);
    status->node_8 = node_registry_state_name(8);
}

/* Print the same status snapshot exposed by the web API. */
void app_control_print_status(void)
{
    app_status_t status;
    app_control_get_status(&status);
    ESP_LOGI(TAG, "state=%s file=%s can_drops=%" PRIu32 " log_drops=%" PRIu32,
             data_logger_state_name(), status.current_file,
             status.can_drops, status.log_drops);
    ESP_LOGI(TAG, "AiM=%s age_ms=%" PRId64 " valid=%" PRIu32 " malformed=%" PRIu32 " discontinuities=%" PRIu32,
             status.aim.state, status.aim.last_frame_age_ms, status.aim.valid_frames,
             status.aim.malformed_frames, status.aim.counter_discontinuities);
    ESP_LOGI(TAG, "nodes: 1=%s 3=%s 4=%s 5=%s 6=%s 7=%s 8=%s",
             status.node_1, status.node_3, status.node_4, status.node_5, status.node_6, status.node_7, status.node_8);
}
