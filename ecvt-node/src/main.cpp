// Wire the standalone servo bus, idle drive owner and serial console at boot.
// Startup sends no servo enable or movement command.
#include "drive/control.h"
#include "can/transport.h"
#include "console/console.h"
#include "esp_log.h"

extern "C" void app_main()
{
    ESP_ERROR_CHECK(servo_can_start());
    ESP_ERROR_CHECK(drive_control_start());
    ESP_ERROR_CHECK(serial_console_start());
    ESP_LOGI("ECVT", "Idle. No servo commands sent. Type help; configure limits before motion.");
}
