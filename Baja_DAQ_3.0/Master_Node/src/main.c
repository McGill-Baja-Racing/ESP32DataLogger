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

#define AUTO_START_DELAY_MS 5000
#define TEST_SAMPLE_COUNT 5

/* Composition root: initializes modules and defines application-level flow. */
static const char *TAG = "Master";

typedef struct {
    uint32_t can_id;
    int32_t timestamp_ms;
    int32_t value;
} test_sample_t;

static const test_sample_t test_samples[TEST_SAMPLE_COUNT] = {
    {CAN_ID_FRONT_BRAKE,     1000, 1234},
    {CAN_ID_REAR_BRAKE,      1001,  567},
    {CAN_ID_BEARING_ENCODER, 1002, -321},
    {CAN_ID_GENERIC_ADC,     1003, 3300},
    {CAN_ID_ENGINE_RPM,      1004, 2750},
};

static bool start_logging(void)
{
    if (!data_logger_start()) return false;
    node_registry_set_monitoring(true);
    ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_start_nodes());
    return true;
}

static bool stop_logging(void)
{
    if (!data_logger_stop()) return false;
    node_registry_set_monitoring(false);
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

static bool inject_test_data(void)
{
    if (data_logger_state() != LOGGER_RUNNING) {
        ESP_LOGW(TAG, "Test data requires a running log");
        return false;
    }
    for (size_t i = 0; i < TEST_SAMPLE_COUNT; i++) {
        can_message_t message = {
            .id = test_samples[i].can_id,
            .dlc = 8,
            .data = ((uint64_t)(uint32_t)test_samples[i].timestamp_ms << 32) |
                    (uint32_t)test_samples[i].value,
        };
        esp_err_t error = can_master_inject_test_message(&message);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Test-data injection failed: %s",
                     esp_err_to_name(error));
            return false;
        }
    }
    ESP_LOGI(TAG, "Injected %u test samples into the CAN receive queue",
             TEST_SAMPLE_COUNT);
    return true;
}

static void handle_can_message(const can_message_t *message)
{
    if (node_registry_is_state_frame(message)) {
        node_registry_update(message);
    } else if (protocol_is_sensor_id(message->id)) {
        node_registry_record_sensor_frame(message);
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
    ESP_ERROR_CHECK(node_registry_init());
    ESP_ERROR_CHECK(can_master_init(handle_can_message));

    /* Force nodes idle before the console and automatic session can start. */
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(can_master_stop_nodes());
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    serial_console_callbacks_t console = {
        .start = start_logging,
        .stop = stop_logging,
        .test_data = inject_test_data,
        .status = print_status,
    };
    ESP_ERROR_CHECK(serial_console_start(&console));
    ESP_ERROR_CHECK(time_beacon_start());
    ESP_ERROR_CHECK(xTaskCreate(auto_start_task, "auto_start", 3072, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Minimal logger ready; node clocks synchronize every 100 ms");
}
