#include "time/time_beacon.h"

#include "can/can_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"

#define TIME_BEACON_PERIOD_MS 100
#define FAILURE_LOG_PERIOD_MS 5000

static const char *TAG = "TimeBeacon";

static void beacon_task(void *argument)
{
    (void)argument;
    uint8_t payload[8];
    esp_err_t previous_error = ESP_OK;
    TickType_t last_failure_log = 0;
    while (true) {
        uint64_t time_us = (uint64_t)esp_timer_get_time();
        for (uint8_t i = 0; i < sizeof(payload); i++) {
            payload[i] = (uint8_t)(time_us >> (8 * i));
        }
        esp_err_t error = can_master_send(CAN_ID_MASTER_TIME, payload, sizeof(payload));
        TickType_t now = xTaskGetTickCount();
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT &&
            (error != previous_error ||
             now - last_failure_log >= pdMS_TO_TICKS(FAILURE_LOG_PERIOD_MS))) {
            ESP_LOGW(TAG, "Beacon failed: %s", esp_err_to_name(error));
            last_failure_log = now;
        } else if (error == ESP_OK && previous_error != ESP_OK) {
            ESP_LOGI(TAG, "Beacon transmission recovered");
        }
        previous_error = error;
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
}

esp_err_t time_beacon_start(void)
{
    return xTaskCreate(beacon_task, "time_beacon", 3072, NULL, 7, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
