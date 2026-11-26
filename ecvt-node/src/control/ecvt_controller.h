#pragma once
#include "app/runtime_internal.h"

float calculate_running_rpm_adjustment(float error_abs);

float calculate_running_target_init_from_throttle(uint32_t throttle_pct);

float clamp_running_target_rpm(float target_rpm, float base_target_rpm);

float calculate_position_from_wheel_rpm(float wheel_rpm,
                                               float target_engine_rpm,
                                               float *out_sheave_pos,
                                               float *out_disp);

extern "C" bool ecvt_fsm_motor_actions_enabled(void);

bool update_transition_counter(uint32_t *counter, bool condition_met);

void moving_median_reset(moving_median_filter_t *filter, uint32_t window_size);

float moving_median_update(moving_median_filter_t *filter, float value, uint32_t window_size);

void issue_running_position_command(uint32_t ts32,
                                           float pos,
                                           float wheel_rpm,
                                           float engine_rpm,
                                           float target_engine_rpm,
                                           bool brake_state,
                                           const char *reason);

void decode_sample(const can_rx_item_t *msg, uint32_t *ts_out, float *rpm_out);

// Synchronous legacy loop, called only by the motor owner; returns on cancellation.
void calc_task(void *arg);
