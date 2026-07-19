#include <inttypes.h>

#include "can/can_master.h"
#include "console/serial_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logger/data_logger.h"
#include "node_state/node_registry.h"
#include "protocol/app_protocol.h"
#include "storage/sd_card.h"
#include "time/time_beacon.h"

#define AUTO_START_DELAY_MS 500

/* Composition root: initializes modules and defines application-level flow. */
static const char *TAG = "Master";

static bool start_logging(void)
{
    if (!data_logger_start()) return false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_start_nodes());
    return true;
}

static bool stop_logging(void)
{
    if (!data_logger_stop()) return false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_stop_nodes());
    return true;
}

static void print_status(void)
{
    ESP_LOGI(TAG, "state=%s file=%s can_drops=%" PRIu32 " log_drops=%" PRIu32,
             data_logger_state_name(), data_logger_path(),
             can_master_rx_drop_count(), data_logger_drop_count());
    ESP_LOGI(TAG, "nodes: 1=%s 4=%s 5=%s 6=%s",
             node_registry_state_name(1), node_registry_state_name(4),
             node_registry_state_name(5), node_registry_state_name(6));
}

static void handle_can_message(const can_message_t *message)
{
    if (node_registry_is_state_frame(message)) {
        node_registry_update(message);
    } else if (protocol_is_sensor_id(message->id)) {
        data_logger_enqueue(message);
    }
}

static void auto_start_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(AUTO_START_DELAY_MS));
    (void)start_logging();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(sd_card_mount());
    ESP_ERROR_CHECK(data_logger_init());
    ESP_ERROR_CHECK(can_master_init(handle_can_message));

    /* Force nodes idle before the console and automatic session can start. */
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_stop_nodes());
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    serial_console_callbacks_t console = {
        .start = start_logging,
        .stop = stop_logging,
        .status = print_status,
    };
    ESP_ERROR_CHECK(serial_console_start(&console));
    ESP_ERROR_CHECK(time_beacon_start());
    ESP_ERROR_CHECK(xTaskCreate(auto_start_task, "auto_start", 3072, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Minimal logger ready; node clocks synchronize every 100 ms");
}
