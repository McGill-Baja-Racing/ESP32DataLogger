#include "drive_state.h"
#include "servo/ECVT_motor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

esp_err_t servo_disable_and_confirm()
{
    puts("Stop: sending 6040=0x0000; drive-managed brake engagement requested.");
    const esp_err_t sent = ecvt_motor_sdo_write_u16(0x6040, 0, 0x0000);
    if (sent != ESP_OK)
        printf("Disable command acknowledgement failed, error=%d; checking actual drive state.\n", sent);

    // A missing acknowledgement can still mean that the write took effect.
    // Confirmation continues even when an operation was cancelled.
    const int64_t deadline = esp_timer_get_time() + 1000000;
    uint16_t status = 0;
    do {
        const esp_err_t read = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
        if (read != ESP_OK) {
            printf("DISABLE UNCONFIRMED: status read failed, error=%d. Retry stop.\n", read);
            return read;
        }
        if ((status & 0x004F) == 0x0040) {
            printf("Drive disable confirmed: statusword=0x%04x. Standstill and mechanical brake lock UNVERIFIED.\n", status);
            return ESP_OK;
        }
        if (status & 0x0008) {
            printf("DISABLE UNCONFIRMED: drive fault, statusword=0x%04x; no fault reset sent.\n", status);
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() < deadline);
    printf("DISABLE UNCONFIRMED: statusword=0x%04x did not reach Switch on disabled. Retry stop.\n", status);
    return ESP_ERR_TIMEOUT;
}
