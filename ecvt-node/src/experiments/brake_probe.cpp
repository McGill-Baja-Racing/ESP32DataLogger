#include "brake_probe.h"
#include "servo/ECVT_motor.h"
#include "servo/velocity_zero.h"
#include "servo/drive_state.h"
#include "motor/runtime.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdio>
#include <cinttypes>

namespace {
std::atomic<bool> disable_pending{false};
esp_err_t wait_state(uint16_t expected)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    do {
        if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE;
        uint16_t status = 0;
        esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
        if (result != ESP_OK) return result;
        if (status & 0x0008) {
            printf("Brake test: drive fault, statusword=0x%04x.\n", status);
            return ESP_ERR_INVALID_STATE;
        }
        if ((status & 0x006F) == expected) {
            printf("Brake test: statusword=0x%04x, state confirmed.\n", status);
            return ESP_OK;
        }
        if (!motor_operation_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t check_zero_velocity()
{
    uint8_t mode = 0, errors = 0;
    uint32_t target = 1;
    esp_err_t result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x60FF, 0, &target);
    if (result != ESP_OK) return result;
    if (mode != 3 || errors || target) {
        printf("Brake test check failed: mode=%u (expected 3), target_velocity_raw=%" PRId32
               " (expected 0), error_register=0x%02x (expected 0). Actual velocity is not checked.\n",
               static_cast<unsigned>(mode), static_cast<int32_t>(target),
               static_cast<unsigned>(errors));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
}

bool brake_probe_restore_pending() { return disable_pending.load(); }

esp_err_t brake_probe_restore()
{
    // Also works with no remembered probe, including after an MCU-only reboot.
    disable_pending.store(true);
    const esp_err_t result = servo_disable_and_confirm();
    if (result == ESP_OK) {
        disable_pending.store(false);
    } else puts("Disable UNCONFIRMED; use servo brake-restore to retry 0x0000.");
    return result;
}

esp_err_t brake_probe_run()
{
    if (disable_pending.load()) return ESP_ERR_INVALID_STATE;
    if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE;
    // Clear retained speed BEFORE selecting mode 3, then verify the mode,
    // zero target, disabled state and error register. No position target.
    esp_err_t result = servo_prepare_velocity_zero();
    if (result != ESP_OK) return result;
    const uint16_t words[] = {0x0006, 0x0007, 0x000F};
    const uint16_t states[] = {0x0021, 0x0023, 0x0027};
    // A failed acknowledgement can still mean that a controlword took effect.
    disable_pending.store(true);
    for (unsigned i = 0; i < 3 && result == ESP_OK; ++i) {
        if (motor_operation_cancelled()) { result = ESP_ERR_INVALID_STATE; break; }
        if (words[i] == 0x000F) {
            puts("Brake test: sending target velocity 60FF=0 immediately before enable.");
            result = ecvt_motor_sdo_write_u32(0x60FF, 0, 0);
            if (result != ESP_OK) break;
        }
        result = check_zero_velocity();
        if (result != ESP_OK) break;
        if (motor_operation_cancelled()) { result = ESP_ERR_INVALID_STATE; break; }
        printf("Brake test: sending 6040=0x%04x.\n", words[i]);
        result = ecvt_motor_sdo_write_u16(0x6040, 0, words[i]);
        if (result == ESP_OK) result = wait_state(states[i]);
    }
    if (result == ESP_OK) {
        puts("BRAKE RELEASE WINDOW: servo enabled at zero target velocity for 10 seconds; observe brake. Motor torque is enabled; actual-velocity checks bypassed.");
        const int64_t deadline = esp_timer_get_time() + 10000000;
        while (result == ESP_OK && esp_timer_get_time() < deadline) {
            if (!motor_operation_wait_ms(50)) { result = ESP_ERR_INVALID_STATE; break; }
            result = check_zero_velocity();
            if (result == ESP_OK) result = wait_state(0x0027);
        }
    }
    const esp_err_t disabled = brake_probe_restore();
    return disabled != ESP_OK ? disabled : result;
}
