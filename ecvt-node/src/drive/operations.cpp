// Owner-only drive sequences: mode preparation, position moves, brake diagnostics
// and unconditional disable. These functions never create a second drive owner.
#include "operations.h"
#include "operation_context.h"
#include "canopen/drive.h"
#include "canopen/feedback.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cinttypes>
#include <initializer_list>

namespace {
// CiA 402 statusword bit 10 indicates target reached; it must be fresh.
constexpr uint16_t TARGET_REACHED_MASK = 0x0400;
constexpr uint16_t DRIVE_STATE_MASK = 0x006F;
constexpr uint16_t DISABLED_STATE_MASK = 0x004F;
constexpr uint16_t SWITCH_ON_DISABLED = 0x0040;
constexpr uint16_t FAULT_MASK = 0x0008;
constexpr int64_t STATE_TRANSITION_TIMEOUT_US = 1000000;
constexpr int64_t MODE_CHANGE_TIMEOUT_US = 500000;
constexpr int64_t MOVE_COMPLETION_TIMEOUT_US = 20000000;
constexpr int64_t BRAKE_WINDOW_US = 10000000;
constexpr int64_t torque_zero_window_us = 20000000;
constexpr float accel_up = 2300, decel_up = 2300, accel_down = 2300, decel_down = 2300;
} // namespace

// Poll a commanded CiA 402 state; faults/cancellation abort an enable sequence.
static esp_err_t wait_for_drive_state(uint16_t expected, uint16_t controlword)
{
    const int64_t deadline = esp_timer_get_time() + STATE_TRANSITION_TIMEOUT_US;
    uint16_t actual = 0;
    while (esp_timer_get_time() < deadline) {
        if (drive_operation_cancelled())
            return ESP_ERR_INVALID_STATE;
        const esp_err_t error = canopen_drive_sdo_read_u16(0x6041, 0, &actual);
        if (error != ESP_OK)
            return error;
        if (actual & FAULT_MASK) {
            printf("Enable stopped: drive fault after controlword=0x%04x statusword=0x%04x.\n",
                   controlword, actual);
            return ESP_FAIL;
        }
        if ((actual & DRIVE_STATE_MASK) == expected) {
            printf("Drive transition confirmed: controlword=0x%04x statusword=0x%04x.\n",
                   controlword, actual);
            return ESP_OK;
        }
        if (!drive_operation_wait_ms(20))
            return ESP_ERR_INVALID_STATE;
    }
    printf("Enable stopped: controlword=0x%04x statusword=0x%04x; expected state=0x%04x (mask "
           "0x006F). No target trigger sent.\n",
           controlword, actual, expected);
    return ESP_ERR_TIMEOUT;
}

// This is a bounded brake-voltage observation. Zero is the only torque target
// it can send, and cleanup writes zero again before disabling the drive.
esp_err_t drive_test_zero_torque(bool &drive_touched)
{
    uint16_t status = 0, target = 1;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = canopen_drive_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK)
        result = canopen_drive_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & DISABLED_STATE_MASK) != SWITCH_ON_DISABLED || errors != 0) {
        printf("Torque-zero test refused: status=0x%04x error_register=0x%02x; requires disabled, "
               "alarm-free drive.\n",
               status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    drive_touched = true;
    result = canopen_drive_sdo_write_u16(0x6071, 0, 0);
    if (result == ESP_OK)
        result = canopen_drive_sdo_write_u8(0x6060, 0, 4);
    const int64_t mode_deadline = esp_timer_get_time() + MODE_CHANGE_TIMEOUT_US;
    while (result == ESP_OK && esp_timer_get_time() < mode_deadline) {
        result = canopen_drive_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 4)
            break;
        if (!drive_operation_wait_ms(20)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    if (result == ESP_OK && mode != 4)
        result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK)
        result = canopen_drive_sdo_read_u16(0x6071, 0, &target);
    if (result == ESP_OK && target != 0)
        result = ESP_FAIL;
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x000F)}) {
        if (result != ESP_OK || drive_operation_cancelled()) {
            if (result == ESP_OK)
                result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = canopen_drive_sdo_write_u16(0x6040, 0, word);
        if (result == ESP_OK)
            result = wait_for_drive_state(word == 0x0006   ? 0x0021
                                          : word == 0x0007 ? 0x0023
                                                           : 0x0027,
                                          word);
    }
    int64_t window_started = 0;
    if (result == ESP_OK) {
        puts("ZERO-TORQUE BRAKE WINDOW: measure BRK+/BRK- now; target torque=0, duration 20 "
             "seconds.");
        window_started = esp_timer_get_time();
        const int64_t deadline = window_started + torque_zero_window_us;
        while (esp_timer_get_time() < deadline && !drive_operation_cancelled()) {
            result = canopen_drive_sdo_read_u16(0x6041, 0, &status);
            if (result != ESP_OK || (status & DRIVE_STATE_MASK) != 0x0027 ||
                (status & FAULT_MASK)) {
                if (result == ESP_OK) {
                    printf("Torque-zero window ended early: statusword=0x%04x.\n", status);
                    result = ESP_FAIL;
                }
                break;
            }
            result = canopen_drive_sdo_read_u16(0x6071, 0, &target);
            if (result != ESP_OK || target != 0) {
                if (result == ESP_OK)
                    result = ESP_FAIL;
                break;
            }
            if (!drive_operation_wait_ms(50)) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
        }
    }
    const esp_err_t zero_result = canopen_drive_sdo_write_u16(0x6071, 0, 0);
    const esp_err_t disable_result = canopen_drive_sdo_write_u16(0x6040, 0, 0);
    if (result == ESP_OK && zero_result != ESP_OK)
        result = zero_result;
    if (result == ESP_OK && disable_result != ESP_OK)
        result = disable_result;
    const int64_t elapsed_ms = window_started ? (esp_timer_get_time() - window_started) / 1000 : 0;
    printf("Torque-zero test cleanup: window_elapsed_ms=%lld target torque zero=%s disable=%s.\n",
           static_cast<long long>(elapsed_ms), esp_err_to_name(zero_result),
           esp_err_to_name(disable_result));
    return result;
}

esp_err_t drive_prepare_position_mode()
{
    uint16_t status = 0;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = canopen_drive_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK)
        result = canopen_drive_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & DISABLED_STATE_MASK) != SWITCH_ON_DISABLED || errors != 0) {
        printf("Position-mode preparation refused: status=0x%04x error_register=0x%02x; requires "
               "disabled drive without alarm.\n",
               status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    result = canopen_drive_sdo_write_u8(0x6060, 0, 1);
    const int64_t deadline = esp_timer_get_time() + MODE_CHANGE_TIMEOUT_US;
    while (result == ESP_OK && esp_timer_get_time() < deadline) {
        result = canopen_drive_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 1)
            break;
        if (!drive_operation_wait_ms(20))
            return ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK && mode != 1)
        result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK)
        puts("Position mode verified: mode=1. No target or controlword was sent; any stored "
             "position target remains unchanged.");
    return result;
}

esp_err_t drive_prepare_position(const DrivePositionSetup &setup, bool &drive_touched)
{
    drive_touched = true;
#define PREPARE(call)                                                                              \
    do {                                                                                           \
        if (drive_operation_cancelled())                                                           \
            return ESP_ERR_INVALID_STATE;                                                          \
        esp_err_t result = (call);                                                                 \
        if (result != ESP_OK)                                                                      \
            return result;                                                                         \
    } while (0)
    PREPARE(canopen_drive_nmt_send(0x80, setup.node_id));
    if (!drive_operation_wait_ms(100))
        return ESP_ERR_INVALID_STATE;
    PREPARE(canopen_drive_configure_rpdo1_map_controlword_only());
    PREPARE(canopen_drive_configure_rpdo2());
    PREPARE(canopen_drive_configure_rpdo3_map_profile_accel_decel());
    PREPARE(canopen_drive_configure_tpdo1_map_statusword_only());
    // Confirmation needs periodic status, not only status-change events.
    PREPARE(canopen_drive_sdo_write_u8(0x1800, 2, 0xFF));
    PREPARE(canopen_drive_sdo_write_u16(0x1800, 3, 200));
    PREPARE(canopen_drive_sdo_write_u16(0x1800, 5, 30));
    PREPARE(canopen_drive_configure_tpdo2_map_position_only());
    PREPARE(canopen_drive_configure_tpdo2_fast_event());
    PREPARE(canopen_drive_configure_tpdo3_map_velocity_only());
    PREPARE(canopen_drive_configure_tpdo3_fast_event());
    PREPARE(canopen_drive_configure_tpdo4_map_current_only());
    PREPARE(canopen_drive_configure_tpdo4_fast_event());
    PREPARE(canopen_drive_sdo_write_u8(0x6060, 0, 1));
    PREPARE(
        canopen_drive_sdo_write_u16(0x605D, 0, 1)); // Halt uses configured profile deceleration.
    PREPARE(canopen_drive_set_directional_profile(accel_up, decel_up, accel_down, decel_down));
    PREPARE(
        canopen_drive_send_profile_accel_decel(setup.acceleration_rpm_s, setup.deceleration_rpm_s));
    const int64_t feedback_start = esp_timer_get_time();
    PREPARE(canopen_drive_nmt_send(0x01, setup.node_id));
    uint8_t actual_mode = 0;
    PREPARE(canopen_drive_sdo_read_u8(0x6061, 0, &actual_mode));
    if (actual_mode != 1)
        return ESP_ERR_INVALID_STATE;
    while (drive_feedback_snapshot().position_us <= feedback_start) {
        if (esp_timer_get_time() - feedback_start > STATE_TRANSITION_TIMEOUT_US)
            return ESP_ERR_TIMEOUT;
        if (!drive_operation_wait_ms(10))
            return ESP_ERR_INVALID_STATE;
    }
    // Seed a stationary target before enabling, so clearing Halt cannot resume
    // an old target from a prior test. No zero capture occurs here.
    PREPARE(canopen_drive_send_target_position_and_speed(
        canopen_drive_get_actual_position_feedback(), fminf(1800, setup.maximum_rpm)));
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x010F)}) {
        PREPARE(canopen_drive_send_controlword(word));
        PREPARE(wait_for_drive_state(word == 0x0006   ? 0x0021
                                     : word == 0x0007 ? 0x0023
                                                      : 0x0027,
                                     word));
    }
#undef PREPARE
    return ESP_OK;
}

esp_err_t drive_prepare_velocity_zero()
{
    uint16_t status = 0;
    uint8_t errors = 0, mode = 0;
    uint32_t target = 1;
#define CHECK(call)                                                                                \
    do {                                                                                           \
        if (drive_operation_cancelled())                                                           \
            return ESP_ERR_INVALID_STATE;                                                          \
        const esp_err_t result = (call);                                                           \
        if (result != ESP_OK)                                                                      \
            return result;                                                                         \
    } while (0)
    CHECK(canopen_drive_sdo_read_u16(0x6041, 0, &status));
    CHECK(canopen_drive_sdo_read_u8(0x1001, 0, &errors));
    if ((status & DISABLED_STATE_MASK) != SWITCH_ON_DISABLED || errors) {
        printf("Velocity-zero preparation refused: status=0x%04x error_register=0x%02x; requires "
               "disabled drive without alarm.\n",
               status, errors);
        return ESP_ERR_INVALID_STATE;
    }
    // Clear a retained speed before selecting the velocity controller.
    CHECK(canopen_drive_sdo_write_u32(0x60FF, 0, 0));
    CHECK(canopen_drive_sdo_write_u8(0x6060, 0, 3));
    const int64_t deadline = esp_timer_get_time() + MODE_CHANGE_TIMEOUT_US;
    do {
        CHECK(canopen_drive_sdo_read_u8(0x6061, 0, &mode));
        if (mode == 3)
            break;
        if (!drive_operation_wait_ms(20))
            return ESP_ERR_INVALID_STATE;
    } while (esp_timer_get_time() < deadline);
    if (mode != 3)
        return ESP_ERR_TIMEOUT;
    CHECK(canopen_drive_sdo_read_u32(0x60FF, 0, &target));
    CHECK(canopen_drive_sdo_read_u16(0x6041, 0, &status));
    CHECK(canopen_drive_sdo_read_u8(0x1001, 0, &errors));
    if (target || errors || (status & DISABLED_STATE_MASK) != SWITCH_ON_DISABLED) {
        puts("Velocity-zero verification failed; no enable/controlword was sent.");
        return ESP_ERR_INVALID_STATE;
    }
    printf("Velocity mode verified: mode=3 target=0 statusword=0x%04x error_register=0. Drive "
           "remains disabled; no controlword sent.\n",
           status);
#undef CHECK
    return ESP_OK;
}

static esp_err_t disable_and_confirm()
{
    puts("Stop: sending 6040=0x0000; drive-managed brake engagement requested.");
    const esp_err_t sent = canopen_drive_sdo_write_u16(0x6040, 0, 0x0000);
    if (sent != ESP_OK)
        printf("Disable command acknowledgement failed, error=%d; checking actual drive state.\n",
               sent);

    // A missing acknowledgement can still mean that the write took effect.
    // Confirmation continues even when an operation was cancelled.
    const int64_t deadline = esp_timer_get_time() + STATE_TRANSITION_TIMEOUT_US;
    uint16_t status = 0;
    do {
        const esp_err_t read = canopen_drive_sdo_read_u16(0x6041, 0, &status);
        if (read != ESP_OK) {
            printf("DISABLE UNCONFIRMED: status read failed, error=%d. Retry stop.\n", read);
            return read;
        }
        if ((status & DISABLED_STATE_MASK) == SWITCH_ON_DISABLED) {
            printf("Drive disable confirmed: statusword=0x%04x. Standstill and mechanical brake "
                   "lock UNVERIFIED.\n",
                   status);
            return ESP_OK;
        }
        if (status & FAULT_MASK) {
            printf("DISABLE UNCONFIRMED: drive fault, statusword=0x%04x; no fault reset sent.\n",
                   status);
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() < deadline);
    printf("DISABLE UNCONFIRMED: statusword=0x%04x did not reach Switch on disabled. Retry stop.\n",
           status);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t move_to(float position, float rpm, float accel, float decel)
{
    if (drive_operation_cancelled())
        return ESP_ERR_INVALID_STATE;
    if (!drive_position_allowed(position, rpm))
        return ESP_ERR_INVALID_ARG;
    esp_err_t err =
        canopen_drive_send_target_position_speed_accel_decel(position, rpm, accel, decel);
    if (err != ESP_OK || drive_operation_cancelled())
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    err = canopen_drive_enable_position();
    return err;
}

// A target-reached bit may belong to a previous move: require fresh feedback
// and either a not-reached transition or a position sample at this target.
static esp_err_t wait_for_move(float position, int64_t started_us)
{
    const int64_t deadline = started_us + MOVE_COMPLETION_TIMEOUT_US;
    bool saw_not_reached = false;
    while (!drive_operation_cancelled() && esp_timer_get_time() < deadline) {
        const auto feedback = drive_feedback_snapshot();
        if (feedback.status_us > started_us) {
            if (feedback.statusword & FAULT_MASK) {
                uint16_t code = 0;
                const esp_err_t read = canopen_drive_sdo_read_u16(0x603F, 0, &code);
                printf(
                    "Drive fault during move: statusword=0x%04x error_code=0x%04x code_read=%s.\n",
                    feedback.statusword, code, esp_err_to_name(read));
                return ESP_FAIL;
            }
            if (!(feedback.statusword & TARGET_REACHED_MASK))
                saw_not_reached = true;
            // A fresh bit alone may describe the previous target. Accept it only
            // after seeing movement/not-reached or fresh position at this target.
            const bool at_position =
                feedback.position_us > started_us &&
                fabsf(canopen_drive_get_actual_position_feedback() - position) <= 0.0001f;
            if ((feedback.statusword & TARGET_REACHED_MASK) && (saw_not_reached || at_position)) {
                printf("Target reached: logical_position=%.4f feedback_counts=%" PRIi32 ".\n",
                       position, feedback.position_counts);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return drive_operation_cancelled() ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

esp_err_t drive_move_to(float position, float rpm, float accel, float decel)
{
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = move_to(position, rpm, accel, decel);
    if (err != ESP_OK)
        return err;
    return wait_for_move(position, started_us);
}

namespace {
std::atomic<bool> disable_pending{false};
esp_err_t wait_state(uint16_t expected)
{
    const int64_t deadline = esp_timer_get_time() + STATE_TRANSITION_TIMEOUT_US;
    do {
        if (drive_operation_cancelled())
            return ESP_ERR_INVALID_STATE;
        uint16_t status = 0;
        esp_err_t result = canopen_drive_sdo_read_u16(0x6041, 0, &status);
        if (result != ESP_OK)
            return result;
        if (status & FAULT_MASK) {
            printf("Brake test: drive fault, statusword=0x%04x.\n", status);
            return ESP_ERR_INVALID_STATE;
        }
        if ((status & DRIVE_STATE_MASK) == expected) {
            printf("Brake test: statusword=0x%04x, state confirmed.\n", status);
            return ESP_OK;
        }
        if (!drive_operation_wait_ms(20))
            return ESP_ERR_INVALID_STATE;
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

// Verify selected mode and commanded target; actual velocity is deliberately not a guard.
esp_err_t check_zero_velocity()
{
    uint8_t mode = 0, errors = 0;
    uint32_t target = 1;
    esp_err_t result = canopen_drive_sdo_read_u8(0x6061, 0, &mode);
    if (result == ESP_OK)
        result = canopen_drive_sdo_read_u8(0x1001, 0, &errors);
    if (result == ESP_OK)
        result = canopen_drive_sdo_read_u32(0x60FF, 0, &target);
    if (result != ESP_OK)
        return result;
    if (mode != 3 || errors || target) {
        printf(
            "Brake test check failed: mode=%u (expected 3), target_velocity_raw=%" PRId32
            " (expected 0), error_register=0x%02x (expected 0). Actual velocity is not checked.\n",
            static_cast<unsigned>(mode), static_cast<int32_t>(target),
            static_cast<unsigned>(errors));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
} // namespace

bool drive_disable_pending()
{
    return disable_pending.load();
}

esp_err_t drive_disable()
{
    // Also works with no remembered probe, including after an MCU-only reboot.
    disable_pending.store(true);
    const esp_err_t result = disable_and_confirm();
    if (result == ESP_OK) {
        disable_pending.store(false);
    } else
        puts("Disable UNCONFIRMED; use servo brake-restore to retry 0x0000.");
    return result;
}

esp_err_t drive_test_brake()
{
    if (disable_pending.load())
        return ESP_ERR_INVALID_STATE;
    if (drive_operation_cancelled())
        return ESP_ERR_INVALID_STATE;
    // Clear retained speed BEFORE selecting mode 3, then verify the mode,
    // zero target, disabled state and error register. No position target.
    esp_err_t result = drive_prepare_velocity_zero();
    if (result != ESP_OK)
        return result;
    const uint16_t words[] = {0x0006, 0x0007, 0x000F};
    const uint16_t states[] = {0x0021, 0x0023, 0x0027};
    // A failed acknowledgement can still mean that a controlword took effect.
    disable_pending.store(true);
    for (unsigned i = 0; i < 3 && result == ESP_OK; ++i) {
        if (drive_operation_cancelled()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (words[i] == 0x000F) {
            puts("Brake test: sending target velocity 60FF=0 immediately before enable.");
            result = canopen_drive_sdo_write_u32(0x60FF, 0, 0);
            if (result != ESP_OK)
                break;
        }
        result = check_zero_velocity();
        if (result != ESP_OK)
            break;
        if (drive_operation_cancelled()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        printf("Brake test: sending 6040=0x%04x.\n", words[i]);
        result = canopen_drive_sdo_write_u16(0x6040, 0, words[i]);
        if (result == ESP_OK)
            result = wait_state(states[i]);
    }
    if (result == ESP_OK) {
        puts("BRAKE RELEASE WINDOW: servo enabled at zero target velocity for 10 seconds; observe "
             "brake. Motor torque is enabled; actual-velocity checks bypassed.");
        const int64_t deadline = esp_timer_get_time() + BRAKE_WINDOW_US;
        while (result == ESP_OK && esp_timer_get_time() < deadline) {
            if (!drive_operation_wait_ms(50)) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
            result = check_zero_velocity();
            if (result == ESP_OK)
                result = wait_state(0x0027);
        }
    }
    const esp_err_t disabled = drive_disable();
    return disabled != ESP_OK ? disabled : result;
}
