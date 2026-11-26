#include "velocity_zero.h"
#include "servo/ECVT_motor.h"
#include "app/ecvt_app.h"
#include "esp_timer.h"
#include <cstdio>

esp_err_t servo_prepare_velocity_zero()
{
    uint16_t status = 0;
    uint8_t errors = 0, mode = 0;
    uint32_t target = 1;
#define CHECK(call) do { if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE; \
    const esp_err_t result = (call); if (result != ESP_OK) return result; } while (0)
    CHECK(ecvt_motor_sdo_read_u16(0x6041, 0, &status));
    CHECK(ecvt_motor_sdo_read_u8(0x1001, 0, &errors));
    if ((status & 0x004F) != 0x0040 || errors) {
        printf("Velocity-zero preparation refused: status=0x%04x error_register=0x%02x; requires disabled drive without alarm.\n", status, errors);
        return ESP_ERR_INVALID_STATE;
    }
    // Clear a retained speed before selecting the velocity controller.
    CHECK(ecvt_motor_sdo_write_u32(0x60FF, 0, 0));
    CHECK(ecvt_motor_sdo_write_u8(0x6060, 0, 3));
    const int64_t deadline = esp_timer_get_time() + 500000;
    do {
        CHECK(ecvt_motor_sdo_read_u8(0x6061, 0, &mode));
        if (mode == 3) break;
        if (!ecvt_app_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    } while (esp_timer_get_time() < deadline);
    if (mode != 3) return ESP_ERR_TIMEOUT;
    CHECK(ecvt_motor_sdo_read_u32(0x60FF, 0, &target));
    CHECK(ecvt_motor_sdo_read_u16(0x6041, 0, &status));
    CHECK(ecvt_motor_sdo_read_u8(0x1001, 0, &errors));
    if (target || errors || (status & 0x004F) != 0x0040) {
        puts("Velocity-zero verification failed; no enable/controlword was sent.");
        return ESP_ERR_INVALID_STATE;
    }
    printf("Velocity mode verified: mode=3 target=0 statusword=0x%04x error_register=0. Drive remains disabled; no controlword sent.\n", status);
#undef CHECK
    return ESP_OK;
}
