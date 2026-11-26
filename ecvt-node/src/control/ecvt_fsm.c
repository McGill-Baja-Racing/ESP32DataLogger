#include "ecvt_fsm.h"
#include "app/ecvt_app.h"
#include "ECVT_motor.h"

#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#ifndef ECVT_FSM_WEAK
#define ECVT_FSM_WEAK __attribute__((weak))
#endif

#define ECVT_ACTION(call) do { \
    if (ecvt_app_stop_requested()) return; \
    esp_err_t action_error = (call); \
    if (action_error != ESP_OK) { ecvt_app_motor_error(action_error); return; } \
} while (0)

#define ECVT_FSM_TAG "ECVT_FSM"
#define ECVT_STOP_WAIT_TIMEOUT_MS 1000
#define ECVT_STOP_POSITION_EPSILON_REVS 0.02f
#define ECVT_STOP_STALL_WINDOW_MS 150

static bool ecvt_fsm_wait_for_position_reached(const ecvt_fsm_user_ctx_t *ctx,
                                               int64_t wait_started_us,
                                               uint32_t timeout_ms)
{
    const int64_t deadline_us = wait_started_us + ((int64_t)timeout_ms * 1000);

    while (!ecvt_app_stop_requested() && esp_timer_get_time() < deadline_us) {
        const int64_t latest_rx_us = *ctx->latest_tpdo1_rx_us;
        const uint16_t statusword = *ctx->latest_tpdo1_statusword;

        if (latest_rx_us >= wait_started_us &&
            (statusword & ECVT_STATUSWORD_TARGET_REACHED) != 0U) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

static bool ecvt_fsm_wait_for_stop_position_or_rezero(const ecvt_fsm_user_ctx_t *ctx,
                                                      int64_t wait_started_us,
                                                      uint32_t timeout_ms,
                                                      float target_pos_revs)
{
    const int64_t deadline_us = wait_started_us + ((int64_t)timeout_ms * 1000);
    float last_pos = ecvt_motor_get_actual_position_feedback();
    int64_t last_motion_us = wait_started_us;

    while (!ecvt_app_stop_requested() && esp_timer_get_time() < deadline_us) {
        const int64_t latest_rx_us = *ctx->latest_tpdo1_rx_us;
        const uint16_t statusword = *ctx->latest_tpdo1_statusword;
        const float current_pos = ecvt_motor_get_actual_position_feedback();
        const float pos_error = fabsf(current_pos - target_pos_revs);

        if (latest_rx_us >= wait_started_us &&
            (statusword & ECVT_STATUSWORD_TARGET_REACHED) != 0U) {
            return true;
        }

        if (fabsf(current_pos - last_pos) > ECVT_STOP_POSITION_EPSILON_REVS) {
            last_motion_us = esp_timer_get_time();
            last_pos = current_pos;
        } else if (pos_error > ECVT_STOP_POSITION_EPSILON_REVS &&
                   (esp_timer_get_time() - last_motion_us) >= ((int64_t)ECVT_STOP_STALL_WINDOW_MS * 1000)) {
            ESP_LOGW(ECVT_FSM_TAG,
                     "Stop motion stalled at %.4f revs while seeking %.4f, capturing new zero",
                     current_pos,
                     target_pos_revs);
            if (ecvt_app_stop_requested()) return false;
            esp_err_t error = ecvt_motor_capture_current_position_as_zero();
            if (error != ESP_OK) { ecvt_app_motor_error(error); return false; }
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ecvt_app_stop_requested()) return false;

    ESP_LOGW(ECVT_FSM_TAG,
             "Stop target %.4f not reached within %u ms, capturing current %.4f as new zero",
             target_pos_revs,
             timeout_ms,
             ecvt_motor_get_actual_position_feedback());
    if (ecvt_app_stop_requested()) return false;
    esp_err_t error = ecvt_motor_capture_current_position_as_zero();
    if (error != ESP_OK) { ecvt_app_motor_error(error); return false; }
    return true;
}

void ecvt_fsm_init(ecvt_fsm_t *fsm, int64_t now_us)
{
    if (!fsm) {
        return;
    }

    fsm->state = ECVT_STATE_STOPPED;
    fsm->entered_at_us = now_us;
}

const char *ecvt_state_name(ecvt_state_t state)
{
    switch (state) {
    case ECVT_STATE_STOPPED:
        return "Stopped";
    case ECVT_STATE_RUNNING:
        return "Running";
    case ECVT_STATE_BRAKE:
        return "Brake";
    case ECVT_STATE_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

bool ECVT_FSM_WEAK ecvt_fsm_motor_actions_enabled(void)
{
    return true;
}

bool ecvt_fsm_transition_to(ecvt_fsm_t *fsm,
                            ecvt_state_t requested_state,
                            int64_t now_us,
                            void *user_ctx)
{
    ecvt_state_t old_state;

    if (!fsm || requested_state == fsm->state) {
        return false;
    }

    old_state = fsm->state;
    fsm->state = requested_state;
    fsm->entered_at_us = now_us;

    ecvt_fsm_dispatch_state_entry(requested_state, old_state, user_ctx);

    return true;
}

void ecvt_fsm_dispatch_state_entry(ecvt_state_t new_state, ecvt_state_t old_state, void *user_ctx)
{
    // Perform any task that need to be done for any state change
    ecvt_fsm_on_transition(new_state, old_state, user_ctx);

    switch (new_state) {
    case ECVT_STATE_STOPPED:
        ecvt_fsm_on_enter_stopped(user_ctx, old_state);
        break;
    case ECVT_STATE_RUNNING:
        ecvt_fsm_on_enter_running(user_ctx, old_state);
        break;
    case ECVT_STATE_BRAKE:
        ecvt_fsm_on_enter_brake(user_ctx, old_state);
        break;
    case ECVT_STATE_ERROR:
        ecvt_fsm_on_enter_error(user_ctx, old_state);
        break;
    default:
        break;
    }
}

void ECVT_FSM_WEAK ecvt_fsm_on_transition(ecvt_state_t new_state, ecvt_state_t old_state, void *user_ctx)
{
    (void)user_ctx;

    ESP_LOGI(ECVT_FSM_TAG,
             "Entered state: %s (from %s)",
             ecvt_state_name(new_state),
             ecvt_state_name(old_state));
}

void ECVT_FSM_WEAK ecvt_fsm_on_enter_stopped(void *user_ctx, ecvt_state_t old_state)
{
    ecvt_fsm_user_ctx_t *ctx = (ecvt_fsm_user_ctx_t *)user_ctx;
    int64_t wait_started_us = 0;

    if (!ecvt_fsm_motor_actions_enabled()) {
        return;
    }

    // Stop motor
    if (old_state == ECVT_STATE_RUNNING) {
        if (!ctx || !ctx->fsm || !ctx->latest_tpdo1_statusword || !ctx->latest_tpdo1_rx_us) {
            ESP_LOGE(ECVT_FSM_TAG, "Missing TPDO1 state/FSM context for stop transition");
            if (ctx && ctx->fsm) {
                (void)ecvt_fsm_transition_to(ctx->fsm, ECVT_STATE_ERROR, esp_timer_get_time(), ctx);
            }
            return;
        }

        // Shift to neutral / logical zero
        ECVT_ACTION(ecvt_motor_send_target_position_speed_directional_profile(0.0f, 3000));
        ECVT_ACTION(ecvt_motor_enable_position());

        wait_started_us = esp_timer_get_time();
        (void)ecvt_fsm_wait_for_stop_position_or_rezero(ctx, wait_started_us, ECVT_STOP_WAIT_TIMEOUT_MS, 0.0f);

        ECVT_ACTION(ecvt_motor_send_controlword(0x0002));
    } else {
        ECVT_ACTION(ecvt_motor_send_controlword(0x0000));
    }
}

void ECVT_FSM_WEAK ecvt_fsm_on_enter_running(void *user_ctx, ecvt_state_t old_state)
{
    (void)user_ctx;
    (void)old_state;

    if (!ecvt_fsm_motor_actions_enabled()) {
        return;
    }

    // Enable Motor
    if (old_state != ECVT_STATE_BRAKE){
        ECVT_ACTION(ecvt_motor_enable_operation_sequence_position());
    }
}

void ECVT_FSM_WEAK ecvt_fsm_on_enter_brake(void *user_ctx, ecvt_state_t old_state)
{
    ecvt_fsm_user_ctx_t *ctx = (ecvt_fsm_user_ctx_t *)user_ctx;
    (void)old_state;
    int64_t wait_started_us = 0;

    if (!ecvt_fsm_motor_actions_enabled()) {
        return;
    }

    if (old_state == ECVT_STATE_RUNNING) {
        if (!ctx || !ctx->fsm || !ctx->latest_tpdo1_statusword || !ctx->latest_tpdo1_rx_us) {
            ESP_LOGE(ECVT_FSM_TAG, "Missing TPDO1 state/FSM context for stop transition");
            if (ctx && ctx->fsm) {
                (void)ecvt_fsm_transition_to(ctx->fsm, ECVT_STATE_ERROR, esp_timer_get_time(), ctx);
            }
            return;
        }

        // Shift to neutral / logical zero
        ECVT_ACTION(ecvt_motor_send_target_position_speed_directional_profile(0.0f, 3000));
        ECVT_ACTION(ecvt_motor_enable_position());

        wait_started_us = esp_timer_get_time();
        (void)ecvt_fsm_wait_for_stop_position_or_rezero(ctx, wait_started_us, ECVT_STOP_WAIT_TIMEOUT_MS, 0.0f);
    } else{
        // Stat Motor
        ECVT_ACTION(ecvt_motor_enable_operation_sequence_position());

        if (!ctx || !ctx->fsm || !ctx->latest_tpdo1_statusword || !ctx->latest_tpdo1_rx_us) {
            ESP_LOGE(ECVT_FSM_TAG, "Missing TPDO1 state/FSM context for stop transition");
            if (ctx && ctx->fsm) {
                (void)ecvt_fsm_transition_to(ctx->fsm, ECVT_STATE_ERROR, esp_timer_get_time(), ctx);
            }
            return;
        }

        // Shift to neutral / logical zero
        ECVT_ACTION(ecvt_motor_send_target_position_speed_directional_profile(0.0f, 3000));
        ECVT_ACTION(ecvt_motor_enable_position());

        wait_started_us = esp_timer_get_time();
        (void)ecvt_fsm_wait_for_stop_position_or_rezero(ctx, wait_started_us, ECVT_STOP_WAIT_TIMEOUT_MS, 0.0f);
    }
}

void ECVT_FSM_WEAK ecvt_fsm_on_enter_error(void *user_ctx, ecvt_state_t old_state)
{
    ecvt_fsm_user_ctx_t *ctx = (ecvt_fsm_user_ctx_t *)user_ctx;
    (void)old_state;
    int64_t wait_started_us = 0;

    if (!ecvt_fsm_motor_actions_enabled()) {
        return;
    }

    // Stop motor
    if (old_state == ECVT_STATE_RUNNING) {
        if (!ctx || !ctx->fsm || !ctx->latest_tpdo1_statusword || !ctx->latest_tpdo1_rx_us) {
            ESP_LOGE(ECVT_FSM_TAG, "Missing TPDO1 state/FSM context for stop transition");
            if (ctx && ctx->fsm) {
                (void)ecvt_fsm_transition_to(ctx->fsm, ECVT_STATE_ERROR, esp_timer_get_time(), ctx);
            }
            return;
        }

        // Shift to neutral / logical zero
        ECVT_ACTION(ecvt_motor_send_target_position_speed_directional_profile(0.0f, 3000));
        ECVT_ACTION(ecvt_motor_enable_position());

        wait_started_us = esp_timer_get_time();
        (void)ecvt_fsm_wait_for_stop_position_or_rezero(ctx, wait_started_us, ECVT_STOP_WAIT_TIMEOUT_MS, 0.0f);

        ECVT_ACTION(ecvt_motor_send_controlword(0x0002));

    }
}
