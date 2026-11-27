#include "app/ecvt_app.h"
#include "can/can_transport.h"
#include "console/serial_console.h"
#include "esp_log.h"

extern "C" void app_main()
{
    ESP_ERROR_CHECK(servo_can_start());
    ESP_ERROR_CHECK(ecvt_app_start());
    ESP_ERROR_CHECK(serial_console_start());
    ESP_LOGI("ECVT", "Idle. No servo commands sent. Type help; configure limits before motion.");
}
