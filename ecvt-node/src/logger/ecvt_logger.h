#pragma once
#include "app/runtime_internal.h"

esp_err_t logger_session_start();
void logger_session_stop();
uint32_t logger_dropped_records();

int32_t rpm_to_centi(float rpm);

bool enqueue_log_record(const log_record_t *record);

void queue_servo_feedback_log(uint32_t record_type, int32_t primary_value);

void queue_sensor_snapshot_log(uint32_t record_type);

void queue_controller_command_log(float command_position_revs,
                                         float command_speed_revs_per_sec,
                                         uint32_t throttle_pct,
                                         float engine_rpm,
                                         float wheel_rpm,
                                         float target_engine_rpm,
                                         bool brake_state);

void close_log_file_if_open(void);

bool reopen_log_file_append(const char *path);

void write_log_block(const uint32_t (*buf)[SAMPLE_SIZE], size_t sample_count);

void save_task(void *arg);

void sd_writer_task(void *arg);

void create_unique_log_filename(char *out_path, size_t max_len);
