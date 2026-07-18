#include "time/time_beacon.h"

#include "can/can_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"

#define TIME_BEACON_PERIOD_MS 100

static const char *TAG = "TimeBeacon";

static void beacon_task(void *argument)
{
    (void)argument;
    uint8_t payload[8];
    while (true) {
        uint64_t time_us = (uint64_t)esp_timer_get_time();
        for (uint8_t i = 0; i < sizeof(payload); i++) {
            payload[i] = (uint8_t)(time_us >> (8 * i));
        }
        esp_err_t error = can_master_send(CAN_ID_MASTER_TIME, payload, sizeof(payload));
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Beacon failed: %s", esp_err_to_name(error));
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
}

esp_err_t time_beacon_start(void)
{
    return xTaskCreate(beacon_task, "time_beacon", 3072, NULL, 7, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
