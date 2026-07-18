#include "console/serial_console.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Console";
static serial_console_callbacks_t app_callbacks;

static void console_task(void *argument)
{
    (void)argument;
    char line[32];
    ESP_LOGI(TAG, "Commands: start, stop, status");
    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (char *p = line; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "start") == 0) {
            (void)app_callbacks.start();
        } else if (strcmp(line, "stop") == 0) {
            (void)app_callbacks.stop();
        } else if (strcmp(line, "status") == 0) {
            app_callbacks.status();
        } else if (line[0]) {
            ESP_LOGW(TAG, "Commands: start, stop, status");
        }
    }
}

esp_err_t serial_console_start(const serial_console_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->start || !callbacks->stop || !callbacks->status) {
        return ESP_ERR_INVALID_ARG;
    }
    app_callbacks = *callbacks;
    return xTaskCreate(console_task, "serial", 3072, NULL, 9, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
