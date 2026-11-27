#include "motor_tests.h"
#include "motor/runtime.h"
#include "servo/ECVT_motor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cinttypes>
#include <cstdio>
#include <cmath>
constexpr uint16_t ECVT_STATUSWORD_TARGET_REACHED=0x0400;
#include "servo/servo_feedback.h"

static esp_err_t move_to(float position, float rpm, float accel, float decel)
{
    if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE;
    if (!motor_position_allowed(position, rpm)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ecvt_motor_send_target_position_speed_accel_decel(position, rpm, accel, decel);
    if (err != ESP_OK || motor_operation_cancelled()) return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    err = ecvt_motor_enable_position();
    return err;
}

static esp_err_t wait_for_move(float position, int64_t started_us)
{
    const int64_t deadline = started_us + 20000000;
    bool saw_not_reached = false;
    while (!motor_operation_cancelled() && esp_timer_get_time() < deadline) {
        const auto feedback = servo_feedback_snapshot();
        if (feedback.status_us > started_us) {
            if (feedback.statusword & 0x0008) {
                uint16_t code = 0;
                const esp_err_t read = ecvt_motor_sdo_read_u16(0x603F, 0, &code);
                printf("Drive fault during move: statusword=0x%04x error_code=0x%04x code_read=%s.\n",
                       feedback.statusword, code, esp_err_to_name(read));
                return ESP_FAIL;
            }
            if (!(feedback.statusword & ECVT_STATUSWORD_TARGET_REACHED)) saw_not_reached = true;
            // A fresh bit alone may describe the previous target. Accept it only
            // after seeing movement/not-reached or fresh position at this target.
            const bool at_position = feedback.position_us > started_us &&
                fabsf(ecvt_motor_get_actual_position_feedback() - position) <= 0.0001f;
            if ((feedback.statusword & ECVT_STATUSWORD_TARGET_REACHED) &&
                (saw_not_reached || at_position)) {
                printf("Target reached: logical_position=%.4f feedback_counts=%" PRIi32 ".\n", position, feedback.position_counts);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return motor_operation_cancelled() ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

esp_err_t motor_test_run(MotorCommand kind, float position, float rpm, float accel, float decel)
{
    const int64_t started_us = esp_timer_get_time();
    if (kind != MotorCommand::Move) return ESP_ERR_INVALID_ARG;
    esp_err_t err = move_to(position, rpm, accel, decel);
    if (err != ESP_OK) return err;
    return wait_for_move(position, started_us);
}
