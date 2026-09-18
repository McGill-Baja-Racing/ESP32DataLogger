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

app_control_result_t app_control_start_logging(void)
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
    if (!data_logger_start()) {
        xSemaphoreGive(lifecycle_mutex);
        return APP_CONTROL_FAILED;
    }
#if MASTER_CAN_ENABLED
    node_registry_set_monitoring(true);
    time_beacon_set_recording(true);
    esp_err_t error = can_master_start_nodes();
#endif
    xSemaphoreGive(lifecycle_mutex);
#if MASTER_CAN_ENABLED
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Logger started, but START broadcast failed: %s",
                 esp_err_to_name(error));
    }
#endif
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
#if MASTER_CAN_ENABLED
    time_beacon_set_recording(false);
    node_registry_set_monitoring(false);
    esp_err_t error = can_master_stop_nodes();
#endif
    xSemaphoreGive(lifecycle_mutex);
#if MASTER_CAN_ENABLED
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Logger stopping, but STOP broadcast failed: %s",
                 esp_err_to_name(error));
    }
#endif
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

void app_control_get_status(app_status_t *status)
{
    if (!status) return;
    status->logger_state = data_logger_state();
    status->current_file = data_logger_path();
    status->can_drops = can_master_rx_drop_count();
    status->log_drops = data_logger_drop_count();
    status->live_enabled = live_data_is_enabled();
    status->node_1 = node_registry_state_name(1);
    status->node_3 = node_registry_state_name(3);
    status->node_4 = node_registry_state_name(4);
    status->node_5 = node_registry_state_name(5);
    status->node_6 = node_registry_state_name(6);
}

void app_control_print_status(void)
{
    app_status_t status;
    app_control_get_status(&status);
    ESP_LOGI(TAG, "state=%s file=%s can_drops=%" PRIu32 " log_drops=%" PRIu32,
             data_logger_state_name(), status.current_file,
             status.can_drops, status.log_drops);
    ESP_LOGI(TAG, "nodes: 1=%s 3=%s 4=%s 5=%s 6=%s",
             status.node_1, status.node_3, status.node_4, status.node_5, status.node_6);
}
