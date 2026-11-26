#include "app/modules_internal.h"
#include "app/ecvt_app.h"

ecvt_fsm_t s_fsm = {ECVT_STATE_STOPPED, 0};
ecvt_fsm_user_ctx_t s_fsm_ctx = {&s_fsm, NULL, NULL};
volatile float s_target_engine_rpm_running_init = (float)TARGET_RUNNING_ENG_RPM_WOT;
volatile float s_target_engine_rpm_running = (float)TARGET_RUNNING_ENG_RPM_WOT;
volatile float s_lowest_gear_motor_pos = 0.0f;
volatile float s_highest_gear_motor_pos = 6.9f;
volatile uint32_t s_calc_moving_median_window_size = CALC_MOVING_MEDIAN_WINDOW_DEFAULT;
volatile bool s_running_entry_needs_lowest_gear = false;
uint32_t s_stopped_to_brake_count = 0;
uint32_t s_stopped_to_running_count = 0;
uint32_t s_brake_release_count = 0;
uint32_t s_running_to_brake_count = 0;
uint32_t s_running_to_stopped_count = 0;

float calculate_running_rpm_adjustment(float error_abs)
{
    float adjustment = error_abs / (float)TARGET_RUNNING_ENG_RPM_ADJ_DIV;

    if (adjustment < (float)TARGET_RUNNING_ENG_RPM_ADJ_MIN) {
        adjustment = (float)TARGET_RUNNING_ENG_RPM_ADJ_MIN;
    }
    if (adjustment > (float)TARGET_RUNNING_ENG_RPM_ADJ_MAX) {
        adjustment = (float)TARGET_RUNNING_ENG_RPM_ADJ_MAX;
    }

    return adjustment;
}

float calculate_running_target_init_from_throttle(uint32_t throttle_pct)
{
    if (throttle_pct >= TARGET_RUNNING_ENG_RPM_THROTTLE_MAX) {
        return (float)TARGET_RUNNING_ENG_RPM_WOT;
    }

    return (float)TARGET_RUNNING_ENG_RPM_IDLE +
           (((float)(TARGET_RUNNING_ENG_RPM_WOT - TARGET_RUNNING_ENG_RPM_IDLE) * (float)throttle_pct) /
            (float)TARGET_RUNNING_ENG_RPM_THROTTLE_MAX);
}

float clamp_running_target_rpm(float target_rpm, float base_target_rpm)
{
    const float min_target = base_target_rpm * 0.8f;
    const float max_target = base_target_rpm * 1.2f;

    if (target_rpm < min_target) {
        return min_target;
    }
    if (target_rpm > max_target) {
        return max_target;
    }

    return target_rpm;
}

float calculate_position_from_wheel_rpm(float wheel_rpm,
                                               float target_engine_rpm,
                                               float *out_sheave_pos,
                                               float *out_disp)
{
    float sheave_pos = 3.016f;

    if (wheel_rpm > 0.0f) {
        sheave_pos = target_engine_rpm / (wheel_rpm * 9.3f);
    }

    if (sheave_pos < 0.4428f) {
        sheave_pos = 0.4428f;
    } else if (sheave_pos > 3.016f) {
        sheave_pos = 3.016f;
    }

    float disp = (3.016f - sheave_pos) / 2.5732f;
    if (disp < 0.0f) {
        disp = 0.0f;
    } else if (disp > 1.0f) {
        disp = 1.0f;
    }

    if (out_sheave_pos) {
        *out_sheave_pos = sheave_pos;
    }
    if (out_disp) {
        *out_disp = disp;
    }

    return s_lowest_gear_motor_pos +
           (disp * (s_highest_gear_motor_pos - s_lowest_gear_motor_pos));
}

extern "C" bool ecvt_fsm_motor_actions_enabled(void)
{
    return !WHEEL_RPM_TEST_MODE && !ecvt_app_stop_requested();
}

bool update_transition_counter(uint32_t *counter, bool condition_met)
{
    if (!counter) {
        return false;
    }

    if (condition_met) {
        if (*counter < STATE_TRANSITION_POINTS_REQUIRED) {
            (*counter)++;
        }
    } else {
        *counter = 0;
    }

    return (*counter >= STATE_TRANSITION_POINTS_REQUIRED);
}

void moving_median_reset(moving_median_filter_t *filter, uint32_t window_size)
{
    if (!filter) {
        return;
    }

    memset(filter->values, 0, sizeof(filter->values));
    filter->count = 0;
    filter->next_index = 0;
    filter->applied_window_size = window_size;
}

float moving_median_update(moving_median_filter_t *filter, float value, uint32_t window_size)
{
    if (!filter) {
        return value;
    }

    if (window_size == 0) {
        window_size = 1;
    } else if (window_size > CALC_MOVING_MEDIAN_WINDOW_MAX) {
        window_size = CALC_MOVING_MEDIAN_WINDOW_MAX;
    }

    if (filter->applied_window_size != window_size) {
        moving_median_reset(filter, window_size);
    }

    if (filter->count < window_size) {
        filter->values[filter->next_index] = value;
        filter->count++;
    } else {
        filter->values[filter->next_index] = value;
    }

    filter->next_index = (filter->next_index + 1U) % window_size;

    float sorted_values[CALC_MOVING_MEDIAN_WINDOW_MAX];
    for (uint32_t i = 0; i < filter->count; ++i) {
        sorted_values[i] = filter->values[i];
    }

    for (uint32_t i = 1; i < filter->count; ++i) {
        float key = sorted_values[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0 && sorted_values[j] > key) {
            sorted_values[j + 1] = sorted_values[j];
            --j;
        }
        sorted_values[j + 1] = key;
    }

    if ((filter->count % 2U) == 0U) {
        const uint32_t upper = filter->count / 2U;
        const uint32_t lower = upper - 1U;
        return (sorted_values[lower] + sorted_values[upper]) * 0.5f;
    }

    return sorted_values[filter->count / 2U];
}

void issue_running_position_command(uint32_t ts32,
                                           float pos,
                                           float wheel_rpm,
                                           float engine_rpm,
                                           float target_engine_rpm,
                                           bool brake_state,
                                           const char *reason)
{
    ESP_LOGI(TAG,
             "[RUN CMD] reason=%s ts=%" PRIu32 " state=%s wheel=%.2f engine=%.2f target=%.2f pos=%.2f gear[highest=%.2f lowest=%.2f]",
             reason,
             ts32,
             ecvt_state_name(s_fsm.state),
             wheel_rpm,
             engine_rpm,
             target_engine_rpm,
             pos,
             s_highest_gear_motor_pos,
             s_lowest_gear_motor_pos);

#if !WHEEL_RPM_TEST_MODE
    if (ecvt_app_stop_requested()) return;
    if (!ecvt_app_allow_move(pos, 2000)) { ecvt_app_motor_error(ESP_ERR_INVALID_ARG); return; }
    esp_err_t error = ecvt_motor_send_target_position_speed_directional_profile(pos, 2000);
    if (error == ESP_OK && !ecvt_app_stop_requested()) error = ecvt_motor_enable_position();
    if (error != ESP_OK) { ecvt_app_motor_error(error); return; }
    if (ecvt_app_stop_requested()) return;
    queue_controller_command_log(pos,
                                 2000.0f,
                                 s_latest_throttle,
                                 engine_rpm,
                                 wheel_rpm,
                                 target_engine_rpm,
                                 brake_state);
#endif
}

void decode_sample(const can_rx_item_t *msg, uint32_t *ts_out, float *rpm_out)
{
    uint64_t packed = 0;
    memcpy(&packed, msg->data, sizeof(packed));

    uint32_t ts32 = (uint32_t)(packed >> 32);
    uint32_t rpm_bits = (uint32_t)(packed & 0xFFFFFFFFu);
    float rpm = 0.0f;

    memcpy(&rpm, &rpm_bits, sizeof(rpm));


    if (ts_out) {
        *ts_out = ts32;
    }
    if (rpm_out) {
        *rpm_out = rpm;
    }
}

void calc_task(void *arg)
{
    (void)arg;
    moving_median_filter_t wheel_rpm_filter = {};
    uint32_t frame_count = 0;
    int64_t last_sample_us = esp_timer_get_time();
    s_stopped_to_brake_count = s_stopped_to_running_count = s_brake_release_count = 0;
    s_running_to_brake_count = s_running_to_stopped_count = 0;
    s_running_entry_needs_lowest_gear = false;
    ecvt_fsm_init(&s_fsm, esp_timer_get_time());
    moving_median_reset(&wheel_rpm_filter, (uint32_t)s_calc_moving_median_window_size);
    s_sample_rpm_enabled = true;
    

    while (!ecvt_app_stop_requested()) {
        can_rx_item_t msg;
        const bool sample_received =
            (xQueueReceive(twai_rx_sample_queue, &msg, pdMS_TO_TICKS(20)) == pdTRUE);

        if (ecvt_app_stop_requested()) break;

        if (!sample_received && esp_timer_get_time() - last_sample_us >= 1000000) {
#if !WHEEL_RPM_TEST_MODE
            logger_session_stop();
            ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_STOPPED,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
#endif
        }

        if (!sample_received) {
            continue;
        }

        last_sample_us = esp_timer_get_time();
        frame_count++;

        

        // --------------- State actions -------------------

        /*
         * Add per-state behavior here.
         *
         * RUNNING:
         * - normal control loop
         *
         * BRAKE:
         * - brake-specific command/output logic
         *
         * ERROR:
         * - fault handling / latched shutdown logic
         */
        const can_sensor_map_entry_t *map_entry = can_sensor_map_find_by_id(msg.id, NODE_ID);
        if (!map_entry) {
            ESP_LOGW(TAG, "No sensor map entry for CAN ID 0x%03" PRIX32, msg.id);
            continue;
        }

#if WHEEL_RPM_TEST_MODE
        if (map_entry->sensor_id == CAN_SENSOR_ENGINE_RPM) {
            uint32_t ts32 = 0;
            float rpm = 0.0f;

            decode_sample(&msg, &ts32, &rpm);
            ESP_LOGI(TAG,
                     "[ENGINE RPM TEST] ts=%" PRIu32 " rpm=%.2f state=%s",
                     ts32,
                     rpm,
                     ecvt_state_name(s_fsm.state));
        }
#endif

        // Check for state change
        if (s_fsm.state == ECVT_STATE_STOPPED){

            // Brake is pressed
            if (map_entry->sensor_id == CAN_SENSOR_BRAKE_STATE &&
                update_transition_counter(&s_stopped_to_brake_count, s_latest_brake_state)) {
                s_stopped_to_running_count = 0;
                s_brake_release_count = 0;
                s_running_to_brake_count = 0;
                s_running_to_stopped_count = 0;
                ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_BRAKE,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
            }
            // Car reaches TARGET RPM
            else if (map_entry->sensor_id == CAN_SENSOR_ENGINE_RPM &&
                     update_transition_counter(&s_stopped_to_running_count,
                                               s_latest_engine_rpm >= TARGET_START_ENG_RPM)) {
                s_target_engine_rpm_running_init =
                    calculate_running_target_init_from_throttle(s_latest_throttle);
                s_target_engine_rpm_running = s_target_engine_rpm_running_init;
                s_stopped_to_brake_count = 0;
                s_brake_release_count = 0;
                s_running_to_brake_count = 0;
                s_running_to_stopped_count = 0;
                ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_RUNNING,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
                s_running_entry_needs_lowest_gear = true;
            }
            continue;
        }

        if (s_fsm.state == ECVT_STATE_BRAKE){


            // Brake is released
            if (map_entry->sensor_id == CAN_SENSOR_BRAKE_STATE &&
                update_transition_counter(&s_brake_release_count, !s_latest_brake_state)) {

                // Car is moving or engineRPM >= TARGET -> RUNNING
                if (s_latest_wheel_rpm > 0 || s_latest_engine_rpm >= TARGET_START_ENG_RPM){
                    s_target_engine_rpm_running_init =
                        calculate_running_target_init_from_throttle(s_latest_throttle);
                    s_target_engine_rpm_running = s_target_engine_rpm_running_init;
                    s_stopped_to_brake_count = 0;
                    s_stopped_to_running_count = 0;
                    s_running_to_brake_count = 0;
                    s_running_to_stopped_count = 0;
                    ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_RUNNING,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
                    s_running_entry_needs_lowest_gear = true;
                } 

                // Car is stopped AND engineRPM < TARGET -> STOPPED
                else if (s_latest_wheel_rpm == 0 && s_latest_engine_rpm < TARGET_START_ENG_RPM){
                    s_stopped_to_brake_count = 0;
                    s_stopped_to_running_count = 0;
                    s_running_to_brake_count = 0;
                    s_running_to_stopped_count = 0;
                    ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_STOPPED,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
                }
                
                continue;
            }
        }

        if (s_fsm.state == ECVT_STATE_ERROR){
            ESP_LOGI(TAG, "ERROR");
        }

        if (s_fsm.state == ECVT_STATE_RUNNING){
            if (s_running_entry_needs_lowest_gear) {
                s_running_entry_needs_lowest_gear = false;
                issue_running_position_command((uint32_t)(esp_timer_get_time() / 1000),
                                               s_lowest_gear_motor_pos,
                                               s_latest_wheel_rpm,
                                               s_latest_engine_rpm,
                                               s_target_engine_rpm_running,
                                               s_latest_brake_state,
                                               "running-entry-lowest-gear");
            }

            // Brake is pressed
            if (map_entry->sensor_id == CAN_SENSOR_BRAKE_STATE &&
                update_transition_counter(&s_running_to_brake_count, s_latest_brake_state)) {
                s_running_entry_needs_lowest_gear = false;
                s_stopped_to_brake_count = 0;
                s_stopped_to_running_count = 0;
                s_brake_release_count = 0;
                s_running_to_stopped_count = 0;
                ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_BRAKE,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
                continue;
            }

            // Car is stopped and rpm < TARGET
            if (map_entry->sensor_id == CAN_SENSOR_ENGINE_RPM &&
                update_transition_counter(&s_running_to_stopped_count,
                                          s_latest_engine_rpm < TARGET_STOP_ENG_RPM)) {
                s_running_entry_needs_lowest_gear = false;
                s_stopped_to_brake_count = 0;
                s_stopped_to_running_count = 0;
                s_brake_release_count = 0;
                s_running_to_brake_count = 0;
                ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_STOPPED,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
                continue;
            }

            if (map_entry->sensor_id == CAN_SENSOR_THROTTLE) {
                s_target_engine_rpm_running_init =
                    calculate_running_target_init_from_throttle(s_latest_throttle);
                s_target_engine_rpm_running =
                    clamp_running_target_rpm(s_target_engine_rpm_running,
                                             s_target_engine_rpm_running_init);

                ESP_LOGI(TAG,
                         "Throttle target RPM init=%.2f throttle=%" PRIu32,
                         s_target_engine_rpm_running_init,
                         s_latest_throttle);
            }

            // if whell rpm, calc and shift to position
            else if (map_entry->sensor_id == CAN_SENSOR_WHEEL_RPM) {
                uint32_t ts32 = 0;
                float raw_rpm = 0.0f;
                decode_sample(&msg, &ts32, &raw_rpm);
                const float median_rpm = moving_median_update(&wheel_rpm_filter,
                                                              raw_rpm,
                                                              (uint32_t)s_calc_moving_median_window_size);
                float sheave_pos = 0.0f;
                float disp = 0.0f;
                const uint32_t command_throttle = s_latest_throttle;
                const float command_engine_rpm = s_latest_engine_rpm;
                const float command_wheel_rpm = median_rpm;
                const float command_target_engine_rpm = s_target_engine_rpm_running;
                const bool command_brake_state = s_latest_brake_state;
                const float pos = (-1) * calculate_position_from_wheel_rpm(median_rpm,
                                                                           command_target_engine_rpm,
                                                                           &sheave_pos,
                                                                           &disp);

                ESP_LOGI(TAG,
                         "[POSITION] ts=%" PRIu32 " rpm_raw=%.2f rpm_median=%.2f window=%" PRIu32 " sheave=%.4f disp=%.4f pos=%.2f",
                         ts32,
                         raw_rpm,
                         median_rpm,
                         (uint32_t)s_calc_moving_median_window_size,
                         sheave_pos,
                         disp,
                         pos);
                (void)command_throttle;
                issue_running_position_command(ts32,
                                               pos,
                                               command_wheel_rpm,
                                               command_engine_rpm,
                                               command_target_engine_rpm,
                                               command_brake_state,
                                               "wheel-rpm-update");
            } 

            // If engine rpm and pos are acheived adjust target rpm
            else if (map_entry->sensor_id == CAN_SENSOR_ENGINE_RPM){

                if ((s_latest_tpdo1_statusword & ECVT_STATUSWORD_TARGET_REACHED) != 0U ){
                    const float rpm_error = s_latest_engine_rpm - s_target_engine_rpm_running;
                    const float rpm_error_abs = fabsf(rpm_error);

                    if (rpm_error_abs <= (float)TARGET_RUNNING_ENG_RPM_THRESHOLD) {
                        ESP_LOGI(TAG,
                                 "Running target held: actual=%.2f target=%.2f",
                                 s_latest_engine_rpm,
                                 s_target_engine_rpm_running);
                    } else {
                        const float adjustment = calculate_running_rpm_adjustment(rpm_error_abs);

                        if (rpm_error > 0.0f) {
                            s_target_engine_rpm_running =
                                (s_target_engine_rpm_running > adjustment)
                                    ? (s_target_engine_rpm_running - adjustment)
                                    : 0.0f;
                        } else {
                            s_target_engine_rpm_running += adjustment;
                        }

                        s_target_engine_rpm_running =
                            clamp_running_target_rpm(s_target_engine_rpm_running,
                                                     s_target_engine_rpm_running_init);

                        ESP_LOGI(TAG,
                                 "Adjusted running target RPM: actual=%.2f target=%.2f adj=%.2f",
                                 s_latest_engine_rpm,
                                 s_target_engine_rpm_running,
                                 adjustment);
                    }
                }

            }

            continue;
        }
        

    }
    

    ESP_LOGI(TAG, "calc_task exiting");
    return;
}
