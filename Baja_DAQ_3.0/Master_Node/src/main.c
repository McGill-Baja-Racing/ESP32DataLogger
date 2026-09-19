#include "app/app_control.h"
#include "can/can_master.h"
#include "console/serial_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "protocol/rpm_pairing.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps/gps_receiver.h"
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

#if MASTER_CAN_ENABLED
static void handle_can_message(const can_message_t *message)
{
    static rpm_pairing_t pairing;
    if (node_registry_is_state_frame(message)) {
        if (message->id == CAN_ID_NODE_STATE_BASE + 4 ||
            message->id == CAN_ID_NODE_STATE_BASE + 5) pairing.valid = false;
        node_registry_update(message);
    } else if (protocol_is_sensor_id(message->id) && message->dlc == 8) {
        /* 0x0BD is now master-derived; ignore legacy node 5 copies. */
        if (message->id == CAN_ID_ENGINE_WHEEL_RPM) return;
        node_registry_record_sensor_frame(message);
        data_logger_enqueue(message);
        live_data_record(message);
        can_message_t paired;
        if (rpm_pairing_update(&pairing, message, esp_timer_get_time(), &paired)) {
            data_logger_enqueue(&paired);
            live_data_record(&paired);
        } else if (message->id == CAN_ID_ENGINE_RPM) {
            live_data_invalidate(CAN_ID_ENGINE_WHEEL_RPM);
        }
    }
}
#endif

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
    ESP_ERROR_CHECK(live_data_init());
    ESP_ERROR_CHECK(node_registry_init());
#if MASTER_CAN_ENABLED
    ESP_ERROR_CHECK(can_master_init(handle_can_message));
#endif
    ESP_ERROR_CHECK(gps_receiver_start(handle_gps_sample));
    ESP_ERROR_CHECK(app_control_init());

#if MASTER_CAN_ENABLED
    /* Force nodes idle before the console and automatic session can start. */
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_stop_nodes());
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif

    serial_console_callbacks_t console = {
        .start = start_logging,
        .stop = stop_logging,
        .status = print_status,
    };
    ESP_ERROR_CHECK(serial_console_start(&console));
#if MASTER_CAN_ENABLED
    ESP_ERROR_CHECK(time_beacon_start());
#else
    ESP_LOGW(TAG, "CAN disabled; running as a GPS-only master");
#endif
    ESP_ERROR_CHECK(xTaskCreate(auto_start_task, "auto_start", 3072, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t web_error = web_server_start();
    if (web_error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi controls unavailable: %s", esp_err_to_name(web_error));
    }
    ESP_LOGI(TAG, "Logger ready; node clocks synchronize every 100 ms");
}
