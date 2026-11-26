#include "motor_tests.h"
#include "app/ecvt_app.h"
#include "app/modules_internal.h"
#include "servo/servo_feedback.h"
#include "experiments/three_revolutions.h"

static esp_err_t move_to(float position, float rpm, float accel, float decel)
{
    if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE;
    if (!ecvt_app_allow_move(position, rpm)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ecvt_motor_send_target_position_speed_accel_decel(position, rpm, accel, decel);
    if (err != ESP_OK || ecvt_app_stop_requested()) return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    err = ecvt_motor_enable_position();
    if (err == ESP_OK) queue_controller_command_log(position, rpm, s_latest_throttle,
        s_latest_engine_rpm, s_latest_wheel_rpm, s_target_engine_rpm_running, s_latest_brake_state);
    return err;
}

static esp_err_t wait_for_move(float position, int64_t started_us, bool require_position)
{
    const int64_t deadline = started_us + 20000000;
    int64_t next_diagnostic_us = started_us;
    bool saw_not_reached = false;
    while (!ecvt_app_stop_requested() && esp_timer_get_time() < deadline) {
        if (require_position && esp_timer_get_time() >= next_diagnostic_us) {
            uint32_t voltage = 0;
            const esp_err_t diagnostic = ecvt_motor_sdo_read_u32(0x6079, 0, &voltage);
            const auto sample = servo_feedback_snapshot();
            printf("Bench sample t_ms=%lld status=0x%04x pos=%" PRId32 " vel_raw=%" PRId32 " voltage_raw=%" PRIu32 " voltage_read=%s\n",
                   (long long)((esp_timer_get_time() - started_us)/1000), sample.statusword, sample.position_counts,
                   sample.velocity_counts, voltage, esp_err_to_name(diagnostic));
            next_diagnostic_us = esp_timer_get_time() + 200000;
        }
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
                (require_position ? at_position : (saw_not_reached || at_position))) {
                printf("Target reached: logical_position=%.4f feedback_counts=%" PRIi32 ".\n", position, feedback.position_counts);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ecvt_app_stop_requested() ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

esp_err_t motor_test_run(CommandKind kind, float position, float rpm, float accel, float decel)
{
    const int64_t started_us = esp_timer_get_time();
    if (kind == CommandKind::BenchHold) {
        uint32_t requested_hold = 0;
        esp_err_t initial_read = ecvt_motor_sdo_read_u32(0x607A, 0, &requested_hold);
        if (initial_read != ESP_OK) return initial_read;
        puts("HOLD ONLY: measure brake DC voltage now; up to 10 seconds, no rotation target. Stop/cancel remains available.");
        while (esp_timer_get_time() - started_us < 10000000) {
            if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE;
            const auto sample = servo_feedback_snapshot();
            if (sample.statusword & 0x0008) {
                uint16_t code = 0;
                const esp_err_t read = ecvt_motor_sdo_read_u16(0x603F, 0, &code);
                printf("Drive fault during stationary hold: status=0x%04x error_code=0x%04x code_read=%s.\n", sample.statusword, code, esp_err_to_name(read));
                return ESP_FAIL;
            }
            if (esp_timer_get_time() - sample.status_us > 200000 || (sample.statusword & 0x006F) != 0x0027)
                return ESP_ERR_INVALID_STATE;
            uint32_t voltage = 0, outputs = 0, demanded = 0;
            uint16_t current = 0;
            esp_err_t result = ecvt_motor_sdo_read_u32(0x6079, 0, &voltage);
            if (result == ESP_OK) result = ecvt_motor_sdo_read_u16(0x6078, 0, &current);
            if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x60FE, 1, &outputs);
            if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x6062, 0, &demanded);
            if (result != ESP_OK) return result;
            if (!hold_demand_matches(requested_hold, demanded)) {
                printf("Hold-only aborted: target=%" PRId32 " demanded=%" PRId32 "; stationary demand not verified.\n",
                       static_cast<int32_t>(requested_hold), static_cast<int32_t>(demanded));
                return ESP_ERR_INVALID_STATE;
            }
            printf("Hold sample t_ms=%lld status=0x%04x pos=%" PRId32 " vel_raw=%" PRId32 " current_raw=%d voltage_raw=%" PRIu32 " outputs=0x%08" PRIx32 "\n",
                   (long long)((esp_timer_get_time() - started_us)/1000), sample.statusword, sample.position_counts,
                   sample.velocity_counts, static_cast<int16_t>(current), voltage, outputs);
            if (!ecvt_app_wait_ms(200)) return ESP_ERR_INVALID_STATE;
        }
        puts("Hold-only measurement finished; stopping and disabling through the bench cleanup sequence.");
        return ESP_OK;
    }
    esp_err_t err = move_to(position, rpm, accel, decel);
    if (err != ESP_OK) return err;
    if (kind == CommandKind::StepResponse) {
        if (!ecvt_app_wait_ms(3000)) return ESP_ERR_INVALID_STATE;
        err = move_to(-10.0f, rpm, accel, decel);
        if (err != ESP_OK) return err;
        return ecvt_app_wait_ms(3000) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (kind == CommandKind::PositionStep)
        return ecvt_app_wait_ms(12000) ? ESP_OK : ESP_ERR_INVALID_STATE;
    return wait_for_move(position, started_us, kind == CommandKind::ThreeRevolutions);
}
