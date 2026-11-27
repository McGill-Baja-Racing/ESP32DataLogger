#include "drive_operations.h"
#include "runtime.h"
#include "servo/ECVT_motor.h"
#include "servo/servo_feedback.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdio>
#include <cinttypes>
#include <initializer_list>

namespace {
constexpr int64_t torque_zero_window_us=20000000;
constexpr float accel_up=2300, decel_up=2300, accel_down=2300, decel_down=2300;
}

esp_err_t wait_for_drive_state(uint16_t expected, uint16_t controlword)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    uint16_t actual = 0;
    while (esp_timer_get_time() < deadline) {
        if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE;
        const esp_err_t error = ecvt_motor_sdo_read_u16(0x6041, 0, &actual);
        if (error != ESP_OK) return error;
        if (actual & 0x0008) {
            printf("Enable stopped: drive fault after controlword=0x%04x statusword=0x%04x.\n", controlword, actual);
            return ESP_FAIL;
        }
        if ((actual & 0x006F) == expected) {
            printf("Drive transition confirmed: controlword=0x%04x statusword=0x%04x.\n", controlword, actual);
            return ESP_OK;
        }
        if (!motor_operation_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    }
    printf("Enable stopped: controlword=0x%04x statusword=0x%04x; expected state=0x%04x (mask 0x006F). No target trigger sent.\n",
           controlword, actual, expected);
    return ESP_ERR_TIMEOUT;
}

// This is a bounded brake-voltage observation. Zero is the only torque target
// it can send, and cleanup writes zero again before disabling the drive.
esp_err_t motor_torque_zero_brake_test(bool &drive_touched)
{
    uint16_t status = 0, target = 1;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & 0x004F) != 0x0040 || errors != 0) {
        printf("Torque-zero test refused: status=0x%04x error_register=0x%02x; requires disabled, alarm-free drive.\n", status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    drive_touched = true;
    result = ecvt_motor_sdo_write_u16(0x6071, 0, 0);
    if (result == ESP_OK) result = ecvt_motor_sdo_write_u8(0x6060, 0, 4);
    const int64_t mode_deadline = esp_timer_get_time() + 500000;
    while (result == ESP_OK && esp_timer_get_time() < mode_deadline) {
        result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 4) break;
        if (!motor_operation_wait_ms(20)) { result = ESP_ERR_INVALID_STATE; break; }
    }
    if (result == ESP_OK && mode != 4) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u16(0x6071, 0, &target);
    if (result == ESP_OK && target != 0) result = ESP_FAIL;
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x000F)}) {
        if (result != ESP_OK || motor_operation_cancelled()) {
            if (result == ESP_OK) result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = ecvt_motor_sdo_write_u16(0x6040, 0, word);
        if (result == ESP_OK) result = wait_for_drive_state(word == 0x0006 ? 0x0021 : word == 0x0007 ? 0x0023 : 0x0027, word);
    }
    int64_t window_started = 0;
    if (result == ESP_OK) {
        puts("ZERO-TORQUE BRAKE WINDOW: measure BRK+/BRK- now; target torque=0, duration 20 seconds.");
        window_started = esp_timer_get_time();
        const int64_t deadline = window_started + torque_zero_window_us;
        while (esp_timer_get_time() < deadline && !motor_operation_cancelled()) {
            result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
            if (result != ESP_OK || (status & 0x006F) != 0x0027 || (status & 0x0008)) {
                if (result == ESP_OK) {
                    printf("Torque-zero window ended early: statusword=0x%04x.\n", status);
                    result = ESP_FAIL;
                }
                break;
            }
            result = ecvt_motor_sdo_read_u16(0x6071, 0, &target);
            if (result != ESP_OK || target != 0) { if (result == ESP_OK) result = ESP_FAIL; break; }
            if (!motor_operation_wait_ms(50)) { result = ESP_ERR_INVALID_STATE; break; }
        }
    }
    const esp_err_t zero_result = ecvt_motor_sdo_write_u16(0x6071, 0, 0);
    const esp_err_t disable_result = ecvt_motor_sdo_write_u16(0x6040, 0, 0);
    if (result == ESP_OK && zero_result != ESP_OK) result = zero_result;
    if (result == ESP_OK && disable_result != ESP_OK) result = disable_result;
    const int64_t elapsed_ms = window_started ? (esp_timer_get_time() - window_started) / 1000 : 0;
    printf("Torque-zero test cleanup: window_elapsed_ms=%lld target torque zero=%s disable=%s.\n",
           static_cast<long long>(elapsed_ms), esp_err_to_name(zero_result), esp_err_to_name(disable_result));
    return result;
}

esp_err_t motor_prepare_position_mode()
{
    uint16_t status = 0;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & 0x004F) != 0x0040 || errors != 0) {
        printf("Position-mode preparation refused: status=0x%04x error_register=0x%02x; requires disabled drive without alarm.\n", status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    result = ecvt_motor_sdo_write_u8(0x6060, 0, 1);
    const int64_t deadline = esp_timer_get_time() + 500000;
    while (result == ESP_OK && esp_timer_get_time() < deadline) {
        result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 1) break;
        if (!motor_operation_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK && mode != 1) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) puts("Position mode verified: mode=1. No target or controlword was sent; any stored position target remains unchanged.");
    return result;
}

esp_err_t motor_prepare_position(const MotorPositionSetup &setup, bool &drive_touched)
{
    drive_touched = true;
#define PREPARE(call) do { if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE; \
    esp_err_t result = (call); if (result != ESP_OK) return result; } while (0)
    PREPARE(ecvt_motor_nmt_send(0x80, setup.node_id));
    if (!motor_operation_wait_ms(100)) return ESP_ERR_INVALID_STATE;
    PREPARE(ecvt_motor_configure_rpdo1_map_controlword_only());
    PREPARE(ecvt_motor_configure_rpdo2());
    PREPARE(ecvt_motor_configure_rpdo3_map_profile_accel_decel());
    PREPARE(ecvt_motor_configure_tpdo1_map_statusword_only());
    // Confirmation needs periodic status, not only status-change events.
    PREPARE(ecvt_motor_sdo_write_u8(0x1800, 2, 0xFF));
    PREPARE(ecvt_motor_sdo_write_u16(0x1800, 3, 200));
    PREPARE(ecvt_motor_sdo_write_u16(0x1800, 5, 30));
    PREPARE(ecvt_motor_configure_tpdo2_map_position_only());
    PREPARE(ecvt_motor_configure_tpdo2_fast_event());
    PREPARE(ecvt_motor_configure_tpdo3_map_velocity_only());
    PREPARE(ecvt_motor_configure_tpdo3_fast_event());
    PREPARE(ecvt_motor_configure_tpdo4_map_current_only());
    PREPARE(ecvt_motor_configure_tpdo4_fast_event());
    PREPARE(ecvt_motor_sdo_write_u8(0x6060, 0, 1));
    PREPARE(ecvt_motor_sdo_write_u16(0x605D, 0, 1)); // Halt uses configured profile deceleration.
    PREPARE(ecvt_motor_set_directional_profile(accel_up, decel_up, accel_down, decel_down));
    PREPARE(ecvt_motor_send_profile_accel_decel(setup.acceleration_rpm_s, setup.deceleration_rpm_s));
    const int64_t feedback_start = esp_timer_get_time();
    PREPARE(ecvt_motor_nmt_send(0x01, setup.node_id));
    uint8_t actual_mode = 0;
    PREPARE(ecvt_motor_sdo_read_u8(0x6061, 0, &actual_mode));
    if (actual_mode != 1) return ESP_ERR_INVALID_STATE;
    while (servo_feedback_snapshot().position_us <= feedback_start) {
        if (esp_timer_get_time() - feedback_start > 1000000) return ESP_ERR_TIMEOUT;
        if (!motor_operation_wait_ms(10)) return ESP_ERR_INVALID_STATE;
    }
    // Seed a stationary target before enabling, so clearing Halt cannot resume
    // an old target from a prior test. No zero capture occurs here.
    PREPARE(ecvt_motor_send_target_position_and_speed(ecvt_motor_get_actual_position_feedback(), fminf(1800, setup.maximum_rpm)));
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x010F)}) {
        PREPARE(ecvt_motor_send_controlword(word));
        PREPARE(wait_for_drive_state(word == 0x0006 ? 0x0021 : word == 0x0007 ? 0x0023 : 0x0027, word));
    }
#undef PREPARE
    return ESP_OK;
}
