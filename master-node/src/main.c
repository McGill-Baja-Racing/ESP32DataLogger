#include <inttypes.h>
#include "aim/aim_adapter.h"

#include "app/app_control.h"
#include "can/can_master.h"
#include "console/serial_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps/gps_receiver.h"
#include "health/node_health.h"
#include "logger/data_logger.h"
#include "live/live_data.h"
#include "node_state/node_registry.h"
#include "protocol/app_protocol.h"
#include "storage/sd_card.h"
#include "time/time_beacon.h"
#include "web/web_server.h"

#define AUTO_START_DELAY_MS 5000
/* Composition root: initializes modules and defines application-level flow. */
static const char *TAG = "Master";

static bool start_logging(void)
{
    return app_control_start_logging(NULL) == APP_CONTROL_OK;
}

static bool stop_logging(void)
{
    return app_control_stop_logging() == APP_CONTROL_OK;
}

static void print_status(void)
{
    app_control_print_status();
}

static void handle_can_message(const can_message_t *message)
{
    /* Route external AiM data separately from our timestamped, DAQ-controlled nodes. */
    if (message->id == AIM_CAN_ID) {
        can_message_t sample;
        if (aim_adapter_receive(message, &sample) && data_logger_state() == LOGGER_RUNNING) {
            data_logger_enqueue(&sample);
            live_data_record(&sample);
        }
        return;
    }
    if (node_registry_is_state_frame(message)) {
        node_registry_update(message);
    } else if (node_health_is_frame(message)) {
        node_registry_record_health_frame(node_health_node_id(message));
        node_health_handle_frame(message);
    } else if (protocol_is_sensor_id(message->id) && message->dlc == 8) {
        uint32_t timestamp_ms = (uint32_t)(message->data >> 32);
        if ((message->id == CAN_ID_ENGINE_SPARK ||
             message->id == CAN_ID_ENGINE_RPM) &&
            (int32_t)timestamp_ms < 0) {
            ESP_LOGW(TAG, "Dropping engine frame 0x%03" PRIX32
                     " with invalid timestamp %" PRIu32,
                     message->id, timestamp_ms);
            node_health_count(HEALTH_BAD_TIMESTAMP, 1, message->id, ESP_OK);
            return;
        }
        node_registry_record_sensor_frame(message);
        data_logger_enqueue(message);
        live_data_record(message);
    }
}

static void handle_gps_sample(const can_message_t *sample)
{
    data_logger_enqueue(sample);
    live_data_record(sample);
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
    ESP_ERROR_CHECK(node_health_init());
    ESP_ERROR_CHECK(live_data_init());
    ESP_ERROR_CHECK(node_registry_init());
    /* The CAN dispatch task uses the recording lock as soon as it starts. */
    ESP_ERROR_CHECK(app_control_init());
    ESP_ERROR_CHECK(can_master_init(handle_can_message));
    ESP_ERROR_CHECK(gps_receiver_start(handle_gps_sample));

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
    esp_err_t web_error = web_server_start();
    if (web_error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi controls unavailable: %s", esp_err_to_name(web_error));
    }
    ESP_LOGI(TAG, "Logger ready; node clocks synchronize every 100 ms");
}
