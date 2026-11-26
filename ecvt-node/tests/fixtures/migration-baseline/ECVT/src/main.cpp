/*
Main ECVT Code.

Tasks:
    - Receive data from CAN Bus and store it in buffer (ISR)


    - Transform raw data into position and send to motor
    - (Setup: Read sample data and put it in buffer
        Loop: buffers data in CAN buffer at the discrete intervals)
    - Reads data from motor and stores it
*/

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "ECVT_motor.h"
#include "can_sensor_map.h"
#include "ecvt_fsm.h"

#include <vector>
#include <string>
#include <math.h>
#include <cstdint>

#include "driver/adc.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

/* --------------------- User config --------------------- */
#define TAG                 "ECVT"
#define WHEEL_RPM_TEST_MODE 0

// -------------------- ADC CONFIG (CHANGE THESE) --------------------
// Pick the ADC unit + channel that corresponds to your chosen GPIO.
// You MUST confirm the mapping for your ESP32-P4 board/pinout.
static const adc1_channel_t WHEEL_ADC_CH_USED  = ADC1_CHANNEL_5;
static const adc1_channel_t ENGINE_ADC_CH_USED = ADC1_CHANNEL_4;
static const adc_atten_t   ADC_ATTEN_USED = ADC_ATTEN_DB_11;

static volatile bool s_sample_rpm_enabled = false;

#define RPM_SAMPLE_PERIOD_MS          2
#define RPM_PUBLISH_INTERVAL_US       50000
#define RPM_ZERO_TIMEOUT_MIN_US      250000
#define RPM_ZERO_TIMEOUT_MULTIPLIER       3
#define RPM_PULSE_LOW_THRESHOLD       1000
#define RPM_PULSE_RESET_THRESHOLD     1200

#define TX_GPIO_NUM         5
#define RX_GPIO_NUM         4
#define BITRATE_BPS         500000

#define NODE_ID             1

#define PATH "/sdcard/lap_data.csv"

#define PULSES_PER_ROTATION     1               // Set this to the number of pulses in one full rotation

static QueueHandle_t twai_rx_raw_queue;
static QueueHandle_t twai_rx_sample_queue;
static QueueHandle_t twai_rx_servo_SDO_queue;
static QueueHandle_t log_record_queue;



static SemaphoreHandle_t motor_ready = NULL;

/* --------------------- Globals --------------------- */
static twai_node_handle_t node_hdl = NULL;

static twai_onchip_node_config_t node_cfg{};
static twai_event_callbacks_t cbs{};

static FILE *logFile = NULL;
static int writeCounter = 0;
static volatile bool stop_logging = false;
static SemaphoreHandle_t log_file_mutex = NULL;

/* Logger buffers + SD writer signalling */
#define SAMPLE_SIZE  11
#define SAMPLES_PER_BLOCK  100
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE * sizeof(uint32_t))

static uint32_t logBuffer[SAMPLES_PER_BLOCK][SAMPLE_SIZE];

enum : uint32_t {
    LOG_RECORD_TYPE_SERVO_TPDO2 = 2,
    LOG_RECORD_TYPE_SERVO_TPDO3 = 3,
    LOG_RECORD_TYPE_SERVO_TPDO4 = 4,
    LOG_RECORD_TYPE_SENSOR_THROTTLE = 10,
    LOG_RECORD_TYPE_SENSOR_ENGINE_RPM = 11,
    LOG_RECORD_TYPE_SENSOR_WHEEL_RPM = 12,
    LOG_RECORD_TYPE_SENSOR_BRAKE_STATE = 13,
    LOG_RECORD_TYPE_CONTROLLER_COMMAND = 100,
};

#define TARGET_START_ENG_RPM  3000
#define TARGET_STOP_ENG_RPM   2000
#define STATE_TRANSITION_POINTS_REQUIRED 3
#define CALC_MOVING_MEDIAN_WINDOW_DEFAULT 3
#define CALC_MOVING_MEDIAN_WINDOW_MAX 16
#define TARGET_RUNNING_ENG_RPM_IDLE 1800
#define TARGET_RUNNING_ENG_RPM_WOT 3000
#define TARGET_RUNNING_ENG_RPM_THROTTLE_MAX 90
#define TARGET_RUNNING_ENG_RPM_THRESHOLD 100
#define TARGET_RUNNING_ENG_RPM_ADJ_MIN 25
#define TARGET_RUNNING_ENG_RPM_ADJ_MAX 250
#define TARGET_RUNNING_ENG_RPM_ADJ_DIV 4

static constexpr float accel_up = 2300.0f;
static constexpr float decel_up = 2300.0f;
static constexpr float accel_down = 2300.0f;
static constexpr float decel_down = 2300.0f;
/* --------------------- Structs --------------------- */
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_rx_item_t;

typedef struct {
    float values[CALC_MOVING_MEDIAN_WINDOW_MAX];
    uint32_t count;
    uint32_t next_index;
    uint32_t applied_window_size;
} moving_median_filter_t;

typedef struct {
    adc1_channel_t channel;
    can_sensor_id_t sensor_id;
    const char *name;
    uint32_t pulses_per_rotation;
    int64_t hard_zero_timeout_us;
} rpm_sensor_task_config_t;

struct ReplayData {
    uint32_t timestamp;
    bool brake_state_valid;
    uint32_t brake_state;
    bool throttle_valid;
    uint32_t throttle;
    bool engine_rpm_valid;
    float engine_rpm;
    bool wheel_rpm_valid;
    float wheel_rpm;
};

typedef struct {
    uint32_t t_us;
    uint32_t record_type;
    int32_t primary_value;
    int32_t secondary_value;
    int32_t command_position_counts;
    int32_t command_speed_counts;
    uint32_t throttle_pct;
    int32_t engine_rpm_centi;
    int32_t wheel_rpm_centi;
    int32_t target_engine_rpm_centi;
    uint32_t brake_state;
} log_record_t;

static std::vector<ReplayData> data;
static ecvt_fsm_t s_fsm = {ECVT_STATE_STOPPED, 0};
static ecvt_fsm_user_ctx_t s_fsm_ctx = {&s_fsm, NULL, NULL};

/* Latest CAN sensor values for use in calc_task and state logic. */
static volatile uint32_t s_latest_throttle = 0;
static volatile float s_latest_engine_rpm = 0.0f;
static volatile float s_latest_wheel_rpm = 0.0f;
static volatile bool s_latest_brake_state = false;
static volatile uint16_t s_latest_tpdo1_statusword = 0;
static volatile int64_t s_latest_tpdo1_rx_us = 0;
static volatile float s_target_engine_rpm_running_init = (float)TARGET_RUNNING_ENG_RPM_WOT;
static volatile float s_target_engine_rpm_running = (float)TARGET_RUNNING_ENG_RPM_WOT;
static volatile float s_lowest_gear_motor_pos = 0.0f;
static volatile float s_highest_gear_motor_pos = 6.9f;
static volatile uint32_t s_calc_moving_median_window_size = CALC_MOVING_MEDIAN_WINDOW_DEFAULT;
static volatile bool s_running_entry_needs_lowest_gear = false;
static uint32_t s_stopped_to_brake_count = 0;
static uint32_t s_stopped_to_running_count = 0;
static uint32_t s_brake_release_count = 0;
static uint32_t s_running_to_brake_count = 0;
static uint32_t s_running_to_stopped_count = 0;

static uint32_t decode_sensor_value_u32(const can_rx_item_t *item)
{
    uint32_t value = 0;
    const uint8_t copy_len = (item->dlc < sizeof(value)) ? item->dlc : sizeof(value);

    memcpy(&value, item->data, copy_len);
    return value;
}

static float decode_sensor_value_f32(const can_rx_item_t *item)
{
    uint32_t value_bits = decode_sensor_value_u32(item);
    float value = 0.0f;

    memcpy(&value, &value_bits, sizeof(value));
    return value;
}

static float calculate_running_rpm_adjustment(float error_abs)
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

static float calculate_running_target_init_from_throttle(uint32_t throttle_pct)
{
    if (throttle_pct >= TARGET_RUNNING_ENG_RPM_THROTTLE_MAX) {
        return (float)TARGET_RUNNING_ENG_RPM_WOT;
    }

    return (float)TARGET_RUNNING_ENG_RPM_IDLE +
           (((float)(TARGET_RUNNING_ENG_RPM_WOT - TARGET_RUNNING_ENG_RPM_IDLE) * (float)throttle_pct) /
            (float)TARGET_RUNNING_ENG_RPM_THROTTLE_MAX);
}

static float clamp_running_target_rpm(float target_rpm, float base_target_rpm)
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

static float calculate_position_from_wheel_rpm(float wheel_rpm,
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
    return !WHEEL_RPM_TEST_MODE;
}

static inline uint32_t can_sensor_id_value(can_sensor_id_t sensor_id)
{
    return can_sensor_map_get_id(sensor_id, NODE_ID);
}

static int32_t rpm_to_centi(float rpm)
{
    return (int32_t)lroundf(rpm * 100.0f);
}

static bool update_transition_counter(uint32_t *counter, bool condition_met)
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

static void moving_median_reset(moving_median_filter_t *filter, uint32_t window_size)
{
    if (!filter) {
        return;
    }

    memset(filter->values, 0, sizeof(filter->values));
    filter->count = 0;
    filter->next_index = 0;
    filter->applied_window_size = window_size;
}

static float moving_median_update(moving_median_filter_t *filter, float value, uint32_t window_size)
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

static void queue_controller_command_log(float command_position_revs,
                                         float command_speed_revs_per_sec,
                                         uint32_t throttle_pct,
                                         float engine_rpm,
                                         float wheel_rpm,
                                         float target_engine_rpm,
                                         bool brake_state);

static void issue_running_position_command(uint32_t ts32,
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
    ESP_ERROR_CHECK(ecvt_motor_send_target_position_speed_directional_profile(pos, 2000));
    ESP_ERROR_CHECK(ecvt_motor_enable_position());
    queue_controller_command_log(pos,
                                 2000.0f,
                                 s_latest_throttle,
                                 engine_rpm,
                                 wheel_rpm,
                                 target_engine_rpm,
                                 brake_state);
#endif
}

static bool enqueue_log_record(const log_record_t *record)
{
    if (stop_logging) {
        return false;
    }

    if (log_record_queue == NULL) {
        return false;
    }

    if (xQueueSend(log_record_queue, record, 1) != pdPASS) {
        ESP_LOGW(TAG, "Log queue FULL - record dropped");
        return false;
    }

    return true;
}

static void queue_servo_feedback_log(uint32_t record_type, int32_t primary_value)
{
    log_record_t record = {};
    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = record_type;
    record.primary_value = primary_value;
    (void)enqueue_log_record(&record);
}

static void queue_sensor_snapshot_log(uint32_t record_type)
{
    log_record_t record = {};

    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = record_type;
    record.throttle_pct = s_latest_throttle;
    record.engine_rpm_centi = rpm_to_centi(s_latest_engine_rpm);
    record.wheel_rpm_centi = rpm_to_centi(s_latest_wheel_rpm);
    record.target_engine_rpm_centi = rpm_to_centi(s_target_engine_rpm_running);
    record.brake_state = s_latest_brake_state ? 1U : 0U;

    switch (record_type) {
    case LOG_RECORD_TYPE_SENSOR_THROTTLE:
        record.primary_value = (int32_t)s_latest_throttle;
        break;
    case LOG_RECORD_TYPE_SENSOR_ENGINE_RPM:
        record.primary_value = rpm_to_centi(s_latest_engine_rpm);
        break;
    case LOG_RECORD_TYPE_SENSOR_WHEEL_RPM:
        record.primary_value = rpm_to_centi(s_latest_wheel_rpm);
        break;
    case LOG_RECORD_TYPE_SENSOR_BRAKE_STATE:
        record.primary_value = s_latest_brake_state ? 1 : 0;
        break;
    default:
        break;
    }

    (void)enqueue_log_record(&record);
}

static void queue_controller_command_log(float command_position_revs,
                                         float command_speed_revs_per_sec,
                                         uint32_t throttle_pct,
                                         float engine_rpm,
                                         float wheel_rpm,
                                         float target_engine_rpm,
                                         bool brake_state)
{
    log_record_t record = {};

    record.t_us = (uint32_t)esp_timer_get_time();
    record.record_type = LOG_RECORD_TYPE_CONTROLLER_COMMAND;
    record.command_position_counts = (int32_t)lroundf(command_position_revs * 10000.0f);
    record.command_speed_counts =
        (int32_t)lroundf(command_speed_revs_per_sec * 10000.0f / 60.0f);
    record.throttle_pct = throttle_pct;
    record.engine_rpm_centi = rpm_to_centi(engine_rpm);
    record.wheel_rpm_centi = rpm_to_centi(wheel_rpm);
    record.target_engine_rpm_centi = rpm_to_centi(target_engine_rpm);
    record.brake_state = brake_state ? 1U : 0U;

    (void)enqueue_log_record(&record);
}
/* --------------------- TWAI RX ISR callback --------------------- */
static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8] = {0};
    twai_frame_t fr{};
    fr.buffer = raw;
    fr.buffer_len = sizeof(raw);


    if (twai_node_receive_from_isr(node, &fr) == ESP_OK) {
        can_rx_item_t item = {0};
        item.id  = fr.header.id;
        item.dlc = fr.header.dlc > 8 ? 8 : fr.header.dlc;
        memcpy(item.data, raw, item.dlc);
        xQueueSendFromISR(twai_rx_raw_queue, &item, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

static void close_log_file_if_open(void)
{
    if (logFile) {
        fflush(logFile);
        fclose(logFile);
        logFile = NULL;
        ESP_LOGI(TAG, "Log file closed.");
    }
}

static bool reopen_log_file_append(const char *path)
{
    FILE *reopened = fopen(path, "ab");
    if (!reopened) {
        ESP_LOGE(TAG, "Failed to reopen log file: %s", path);
        return false;
    }

    logFile = reopened;
    ESP_LOGI(TAG, "Log file reopened.");
    return true;
}
/* --------------------- RX dispatch task ------------------ */
static void rx_dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        can_rx_item_t item;
        if (xQueueReceive(twai_rx_raw_queue, &item, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        const can_sensor_map_entry_t *map_entry = can_sensor_map_find_by_id(item.id, NODE_ID);
        if (!map_entry) {
            continue;
        }

        if (map_entry->sensor_id == CAN_SENSOR_SERVO_SDO_RX) {
            if (xQueueSend(twai_rx_servo_SDO_queue, &item, 1) != pdPASS) {
                ESP_LOGW(TAG, "Servo Queue FULL — item dropped");
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO1) {
            if (item.dlc >= 2) {
                s_latest_tpdo1_statusword = (uint16_t)(item.data[0] | (item.data[1] << 8));
                s_latest_tpdo1_rx_us = esp_timer_get_time();
            }
        }
        else if (map_entry->message_type == CAN_SENSOR_MAP_TYPE_SENSOR_DATA) {
            const uint32_t sensor_value = decode_sensor_value_u32(&item);

            switch (map_entry->sensor_id) {
            case CAN_SENSOR_ENGINE_RPM:
                s_latest_engine_rpm = decode_sensor_value_f32(&item);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_ENGINE_RPM);
                }
                break;
            case CAN_SENSOR_BRAKE_STATE:
                s_latest_brake_state = (sensor_value != 0U);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_BRAKE_STATE);
                }
                break;
            case CAN_SENSOR_THROTTLE:
                s_latest_throttle = sensor_value;
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_THROTTLE);
                }
                break;
            case CAN_SENSOR_WHEEL_RPM:
                s_latest_wheel_rpm = decode_sensor_value_f32(&item);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_WHEEL_RPM);
                }
                break;
            default:
                break;
            }

            if (xQueueSend(twai_rx_sample_queue, &item, 1) != pdPASS) {
                ESP_LOGW(TAG, "Sample Queue FULL — item dropped");
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO2) {
            if (item.dlc >= 4) {
                const int32_t position_counts = (int32_t)decode_sensor_value_u32(&item);
                (void)ecvt_motor_set_actual_position_feedback((float)position_counts / 10000.0f);
            }
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO2,
                                         item.dlc >= 4 ? (int32_t)decode_sensor_value_u32(&item) : 0);
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO3) {
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO3,
                                         item.dlc >= 4 ? (int32_t)decode_sensor_value_u32(&item) : 0);
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO4) {
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO4,
                                         item.dlc >= 2 ? (int32_t)(int16_t)decode_sensor_value_u32(&item) : 0);
            }
        }
    }
}

/* ---------------------- Helpers ----------------------*/

static bool enqueue_replay_sensor(can_sensor_id_t sensor_id, uint32_t timestamp, uint32_t value)
{
    can_rx_item_t item = {0};
    const uint64_t packed_data = ((uint64_t)timestamp << 32) | (uint64_t)value;

    item.id = can_sensor_id_value(sensor_id);
    item.dlc = 8;
    memcpy(item.data, &packed_data, sizeof(packed_data));

    if (xQueueSend(twai_rx_raw_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "RAW queue full — dropping %s sample", can_sensor_map_name(sensor_id));
        return false;
    }

    return true;
}

static bool enqueue_replay_sensor_f32(can_sensor_id_t sensor_id, uint32_t timestamp, float value)
{
    uint32_t value_bits = 0;

    memcpy(&value_bits, &value, sizeof(value_bits));
    return enqueue_replay_sensor(sensor_id, timestamp, value_bits);
}

static void configure_rpm_sensor_adc_channels(void)
{
    ESP_ERROR_CHECK(adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(WHEEL_ADC_CH_USED, ADC_ATTEN_USED));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ENGINE_ADC_CH_USED, ADC_ATTEN_USED));
}

static bool parse_optional_u32_field(const char *field, uint32_t *out_value, bool *out_valid)
{
    char *endptr = NULL;
    unsigned long parsed = 0;

    while (*field == ' ' || *field == '\t') {
        field++;
    }

    if (*field == '\0') {
        *out_valid = false;
        *out_value = 0;
        return true;
    }

    parsed = strtoul(field, &endptr, 10);
    if (field == endptr) {
        return false;
    }

    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }

    if (*endptr != '\0') {
        return false;
    }

    *out_valid = true;
    *out_value = (uint32_t)parsed;
    return true;
}

static bool parse_optional_f32_field(const char *field, float *out_value, bool *out_valid)
{
    char *endptr = NULL;
    float parsed = 0.0f;

    while (*field == ' ' || *field == '\t') {
        field++;
    }

    if (*field == '\0') {
        *out_valid = false;
        *out_value = 0.0f;
        return true;
    }

    parsed = strtof(field, &endptr);
    if (field == endptr) {
        return false;
    }

    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }

    if (*endptr != '\0') {
        return false;
    }

    *out_valid = true;
    *out_value = parsed;
    return true;
}

static bool parse_replay_row(char *line, ReplayData *row)
{
    char *cursor = line;
    char *field_start = line;
    char *fields[5] = {0};
    int field_count = 0;
    uint32_t timestamp = 0;
    char *endptr = NULL;

    while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
        if (*cursor == ',') {
            *cursor = '\0';
            if (field_count < 5) {
                fields[field_count++] = field_start;
            }
            field_start = cursor + 1;
        }
        cursor++;
    }

    *cursor = '\0';
    if (field_count < 5) {
        fields[field_count++] = field_start;
    }

    if (field_count != 5) {
        return false;
    }

    while (*fields[0] == ' ' || *fields[0] == '\t') {
        fields[0]++;
    }

    timestamp = (uint32_t)strtoul(fields[0], &endptr, 10);
    if (fields[0] == endptr) {
        return false;
    }
    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }
    if (*endptr != '\0') {
        return false;
    }

    row->timestamp = timestamp;

    return parse_optional_u32_field(fields[1], &row->brake_state, &row->brake_state_valid) &&
           parse_optional_u32_field(fields[2], &row->throttle, &row->throttle_valid) &&
           parse_optional_f32_field(fields[3], &row->engine_rpm, &row->engine_rpm_valid) &&
           parse_optional_f32_field(fields[4], &row->wheel_rpm, &row->wheel_rpm_valid);
}

/* ---------------------- Tasks ----------------------*/

static void read_sample_data(void *arg) {
    ESP_LOGI(TAG, "Opening data file: %s", PATH);

    FILE *f = fopen(PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file");
        vTaskDelete(NULL);
        return;
    }

    // Skip header line.
    char line[128];
    if (!fgets(line, sizeof(line), f)) {
        ESP_LOGE(TAG, "File is empty or header read failed");
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    data.clear();

    // Read each line
    while (fgets(line, sizeof(line), f)) {
        ReplayData row = {};

        if (parse_replay_row(line, &row)) {
            data.push_back(row);
        } else {
            ESP_LOGW(TAG, "Skipping malformed line: %s", line);
        }
    }

    fclose(f);

    uint32_t time_to_wait = 1; //ms
    TickType_t lastWakeTime = xTaskGetTickCount();

    // Wait for motor to be ready before sending data
    (void)xSemaphoreTake(motor_ready, portMAX_DELAY);

    // Buffer and print loaded data in time incremenets
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i].brake_state_valid) {
            (void)enqueue_replay_sensor(CAN_SENSOR_BRAKE_STATE, data[i].timestamp, data[i].brake_state);
        }
        if (data[i].throttle_valid) {
            (void)enqueue_replay_sensor(CAN_SENSOR_THROTTLE, data[i].timestamp, data[i].throttle);
        }
        if (data[i].engine_rpm_valid) {
            (void)enqueue_replay_sensor_f32(CAN_SENSOR_ENGINE_RPM, data[i].timestamp, data[i].engine_rpm);
        }

        if (i < data.size() - 1){
            // Time bewteen current datapoint and next data point
            time_to_wait = data[i + 1].timestamp - data[i].timestamp;
        } else{
            break; // list is over
        }
        
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(time_to_wait));
    }

    // If this task is done, delete it
    vTaskDelete(NULL);
}

static void decode_sample(const can_rx_item_t *msg, uint32_t *ts_out, float *rpm_out)
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


static void calc_task(void *arg)
{
    // Servo Driver Setup
    (void)arg;
    moving_median_filter_t wheel_rpm_filter = {};

#if !WHEEL_RPM_TEST_MODE
    // Go pre-op
    ESP_ERROR_CHECK(ecvt_motor_nmt_send(0x80, (uint8_t)NODE_ID));
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Configure PDO mappings
    ESP_ERROR_CHECK(ecvt_motor_configure_rpdo1_map_controlword_only());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo1_map_statusword_only());
    ESP_ERROR_CHECK(ecvt_motor_configure_rpdo2());
    ESP_ERROR_CHECK(ecvt_motor_configure_rpdo3_map_profile_accel_decel());

    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo2_map_position_only());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo2_fast_event());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo3_map_velocity_only());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo3_fast_event());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo4_map_current_only());
    ESP_ERROR_CHECK(ecvt_motor_configure_tpdo4_fast_event());

    // Start node
    ESP_ERROR_CHECK(ecvt_motor_nmt_send(0x01, (uint8_t)NODE_ID));
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Profile Position mode (1)
    ESP_ERROR_CHECK(ecvt_motor_sdo_write_u8(0x6060, 0x00, 1));
    ESP_ERROR_CHECK(ecvt_motor_set_directional_profile(accel_up, decel_up, accel_down, decel_down));

    ESP_LOGI(TAG, "Configure initial profile velocity via SDO");
    ESP_ERROR_CHECK(ecvt_motor_sdo_write_u32(0x6081, 0x00, (uint32_t)(1800 * 10000 / 60))); // Profile velocity
#endif

    uint32_t frame_count = 0;
    ecvt_fsm_init(&s_fsm, esp_timer_get_time());
    moving_median_reset(&wheel_rpm_filter, (uint32_t)s_calc_moving_median_window_size);
    s_sample_rpm_enabled = true;
#if !WHEEL_RPM_TEST_MODE
    xSemaphoreGive(motor_ready);
#endif
    

    while (1) {
        can_rx_item_t msg;
        const bool sample_received =
            (xQueueReceive(twai_rx_sample_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE);

        if (!sample_received) {
#if !WHEEL_RPM_TEST_MODE
            stop_logging = true;
            ecvt_fsm_transition_to(&s_fsm,
                                   ECVT_STATE_STOPPED,
                                   esp_timer_get_time(),
                                   &s_fsm_ctx);
#endif
        }

        if (!sample_received) {
            continue;
        }

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
    vTaskDelete(NULL);
}

static void write_log_block(const uint32_t (*buf)[SAMPLE_SIZE], size_t sample_count)
{
    const size_t bytes = sample_count * SAMPLE_SIZE * sizeof(uint32_t);
    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        return;
    }

    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Log file is not open");
        return;
    }

    size_t written = fwrite(buf, 1, bytes, logFile);
    fflush(logFile);
    xSemaphoreGive(log_file_mutex);

    if (written != bytes) {
        ESP_LOGE(TAG, "Short write! wrote=%u expected=%u",
                 (unsigned)written, (unsigned)bytes);
        return;
    }

    writeCounter++;
    ESP_LOGI(TAG, "Wrote block %d (%u bytes, %u samples)",
             writeCounter, (unsigned)bytes, (unsigned)sample_count);
}

static void save_task(void *arg)
{
    const char *path = (const char *)arg;
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(15000));

        if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to lock log file mutex");
            continue;
        }

        if (logFile) {
            ESP_LOGI(TAG, "File SAVED");
            close_log_file_if_open();
            if (!reopen_log_file_append(path)) {
                stop_logging = true;
            }
        } else if (stop_logging) {
            xSemaphoreGive(log_file_mutex);
            ESP_LOGI(TAG, "save_task done.");
            vTaskDelete(NULL);
            return;
        }

        xSemaphoreGive(log_file_mutex);
    }
}

static void sample_rpm_task(void *arg)
{
    /*
    This task samples the ADC pulse, detects each pulse crossing, and computes RPM
    from the time between the current pulse and the previous one.
    */

    const rpm_sensor_task_config_t *cfg = (const rpm_sensor_task_config_t *)arg;
    bool peak_detected = false;
    int64_t last_pulse_us = 0;
    int64_t last_delta_us = 0;
    int64_t last_log_us = 0;
    int64_t last_publish_us = 0;
    float last_published_rpm = -1.0f;
    int raw = 0;

    while (!s_sample_rpm_enabled) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    while (1)
    {
        raw = adc1_get_raw(cfg->channel);
        const int64_t t_main_sample_us = esp_timer_get_time();

        // Detect one pulse when the signal crosses below the threshold.
        if (raw < RPM_PULSE_LOW_THRESHOLD && !peak_detected) {
            peak_detected = true;
            int64_t delta_us = 0;
            float rpm = 0.0f;

            if (last_pulse_us != 0) {
                delta_us = t_main_sample_us - last_pulse_us;
                if (delta_us > 0) {
                    last_delta_us = delta_us;
                    rpm = (float)(60000000.0 / ((double)delta_us * (double)cfg->pulses_per_rotation));
                }
            }

            last_pulse_us = t_main_sample_us;

            if (delta_us > 0 && (t_main_sample_us - last_log_us) >= 100000) {
                ESP_LOGI(TAG, "%s pulse detected | raw=%d dt=%lld us rpm=%.2f",
                         cfg->name, raw, delta_us, rpm);
                last_log_us = t_main_sample_us;
            }

            if (delta_us > 0 && (t_main_sample_us - last_publish_us) >= RPM_PUBLISH_INTERVAL_US) {
                const uint32_t timestamp_ms = (uint32_t)(t_main_sample_us / 1000);
                if (enqueue_replay_sensor_f32(cfg->sensor_id, timestamp_ms, rpm)) {
                    last_publish_us = t_main_sample_us;
                    last_published_rpm = rpm;
                }
            } else if (delta_us == 0) {
                ESP_LOGI(TAG, "%s first pulse detected | raw=%d waiting for next pulse",
                         cfg->name, raw);
            }
        }
        // Use a higher reset threshold to avoid retriggering on noisy edges.
        else if (raw > RPM_PULSE_RESET_THRESHOLD && peak_detected) {
            peak_detected = false;
        }

        int64_t zero_timeout_us = RPM_ZERO_TIMEOUT_MIN_US;
        if (cfg->hard_zero_timeout_us > 0) {
            zero_timeout_us = cfg->hard_zero_timeout_us;
        } else if (last_delta_us > 0) {
            const int64_t adaptive_timeout_us = last_delta_us * RPM_ZERO_TIMEOUT_MULTIPLIER;
            if (adaptive_timeout_us > zero_timeout_us) {
                zero_timeout_us = adaptive_timeout_us;
            }
        }

        if (last_pulse_us != 0 &&
            (t_main_sample_us - last_pulse_us) >= zero_timeout_us &&
            (t_main_sample_us - last_publish_us) >= RPM_PUBLISH_INTERVAL_US &&
            last_published_rpm != 0.0f) {
            const uint32_t timestamp_ms = (uint32_t)(t_main_sample_us / 1000);
            if (enqueue_replay_sensor_f32(cfg->sensor_id, timestamp_ms, 0.0f)) {
                last_publish_us = t_main_sample_us;
                last_published_rpm = 0.0f;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));
    }
}

static void sd_writer_task(void *arg)
{
    const char *path = (const char *)arg;
    size_t sampleIndex = 0;

    ESP_LOGI(TAG, "Creating log file: %s", path);
    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        stop_logging = true;
        vTaskDelete(NULL);
        return;
    }

    logFile = fopen(path, "wb");
    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Failed to open log file");
        stop_logging = true;
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(log_file_mutex);

    while (1) {
        log_record_t msg;
        BaseType_t got_sample = xQueueReceive(log_record_queue, &msg, pdMS_TO_TICKS(100));

        if (got_sample == pdTRUE) {
            if (!stop_logging) {
                logBuffer[sampleIndex][0] = msg.t_us;
                logBuffer[sampleIndex][1] = msg.record_type;
                logBuffer[sampleIndex][2] = (uint32_t)msg.primary_value;
                logBuffer[sampleIndex][3] = (uint32_t)msg.secondary_value;
                logBuffer[sampleIndex][4] = (uint32_t)msg.command_position_counts;
                logBuffer[sampleIndex][5] = (uint32_t)msg.command_speed_counts;
                logBuffer[sampleIndex][6] = msg.throttle_pct;
                logBuffer[sampleIndex][7] = (uint32_t)msg.engine_rpm_centi;
                logBuffer[sampleIndex][8] = (uint32_t)msg.wheel_rpm_centi;
                logBuffer[sampleIndex][9] = (uint32_t)msg.target_engine_rpm_centi;
                logBuffer[sampleIndex][10] = msg.brake_state;
                sampleIndex++;

                if (sampleIndex >= SAMPLES_PER_BLOCK) {
                    write_log_block(logBuffer, sampleIndex);
                    sampleIndex = 0;
                }
            }
        }

        if (stop_logging && uxQueueMessagesWaiting(log_record_queue) == 0) {
            if (sampleIndex > 0) {
                ESP_LOGI(TAG, "Flushing partial block (%u samples)...", (unsigned)sampleIndex);
                write_log_block(logBuffer, sampleIndex);
            }

            (void)xSemaphoreTake(log_file_mutex, portMAX_DELAY);
            close_log_file_if_open();
            xSemaphoreGive(log_file_mutex);

            ESP_LOGI(TAG, "sd_writer_task done (one-shot).");
            vTaskDelete(NULL);
        }
    }
}

static void create_unique_log_filename(char *out_path, size_t max_len)
{
    int index = 1;
    struct stat st;
    while (1) {
        snprintf(out_path, max_len, "/sdcard/log_%04d.bin", index);
        if (stat(out_path, &st) != 0) return; // doesn't exist => ok
        index++;
    }
}

extern "C" {
    void app_main(void);
}

static TaskHandle_t sd_task_handle     = NULL;
static TaskHandle_t save_task_handle   = NULL;
static char log_path[64] = {0};
static const rpm_sensor_task_config_t s_wheel_rpm_task_cfg = {
    WHEEL_ADC_CH_USED,
    CAN_SENSOR_WHEEL_RPM,
    "Wheel RPM",
    PULSES_PER_ROTATION,
    15000000
};
static const rpm_sensor_task_config_t s_engine_rpm_task_cfg = {
    ENGINE_ADC_CH_USED,
    CAN_SENSOR_ENGINE_RPM,
    "Engine RPM",
    1,
    0
};

void app_main() {
#if !WHEEL_RPM_TEST_MODE
    /* --------- SD mount --------- */
    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // Power SD IO via on-chip LDO channel 4
    sd_pwr_ctrl_ldo_config_t cfg = {};
    cfg.ldo_chan_id = 4;

    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&cfg, &pwr);
    if (pwr_err == ESP_OK) {
        host.pwr_ctrl_handle = pwr;
        ESP_LOGI(TAG, "SD IO power control enabled (on-chip LDO chan %d)", cfg.ldo_chan_id);
    } else {
        ESP_LOGW(TAG, "SD IO power control NOT enabled: %s", esp_err_to_name(pwr_err));
    }
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    /* P4 SDMMC pins */
    slot_config.clk = (gpio_num_t)43;
    slot_config.cmd = (gpio_num_t)44;
    slot_config.d0  = (gpio_num_t)39;
    slot_config.d1  = (gpio_num_t)40;
    slot_config.d2  = (gpio_num_t)41;
    slot_config.d3  = (gpio_num_t)42;

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;

    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return;
    }
    sdmmc_card_print_info(stdout, card);

    /* Prepare unique log path + start with logging OFF */
    create_unique_log_filename(log_path, sizeof(log_path));
    ESP_LOGI(TAG, "Log path: %s", log_path);
#endif

    // CAN Setup
    node_cfg.io_cfg.tx = (gpio_num_t) TX_GPIO_NUM;
    node_cfg.io_cfg.rx = (gpio_num_t) RX_GPIO_NUM;
    node_cfg.bit_timing.bitrate = BITRATE_BPS;
    node_cfg.tx_queue_depth = 8;

    cbs.on_rx_done = twai_rx_cb;

    twai_rx_raw_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
    twai_rx_sample_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
#if !WHEEL_RPM_TEST_MODE
    twai_rx_servo_SDO_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
    log_record_queue  = xQueueCreate(256, sizeof(log_record_t));
    s_fsm_ctx.latest_tpdo1_statusword = &s_latest_tpdo1_statusword;
    s_fsm_ctx.latest_tpdo1_rx_us = &s_latest_tpdo1_rx_us;

    motor_ready = xSemaphoreCreateBinary();
    log_file_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &node_hdl));
    ESP_LOGI(TAG, "TWAI node created");

    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_LOGI(TAG, "TWAI callbacks registered");

    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled (bitrate=%d, NodeID=%d)", BITRATE_BPS, NODE_ID);
    ESP_ERROR_CHECK(ecvt_motor_init(node_hdl, twai_rx_servo_SDO_queue, NODE_ID, TAG));
#endif
    configure_rpm_sensor_adc_channels();

#if !WHEEL_RPM_TEST_MODE
    xTaskCreate(read_sample_data, "read_sample_data", 8192, NULL, 5, NULL);
#endif
    xTaskCreate(sample_rpm_task, "wheel_rpm", 4096, (void *)&s_wheel_rpm_task_cfg, 5, NULL);
    xTaskCreate(sample_rpm_task, "engine_rpm", 4096, (void *)&s_engine_rpm_task_cfg, 5, NULL);
    xTaskCreate(calc_task, "calc_pos", 8192, NULL, 6, NULL);
    xTaskCreate(rx_dispatch_task, "rx_dispatch", 4096, NULL, 7, NULL);
#if !WHEEL_RPM_TEST_MODE
    xTaskCreate(sd_writer_task,   "sd_writer",   8192, (void *)log_path, 8, &sd_task_handle);
    xTaskCreate(save_task,        "save_task",   4096, (void *)log_path, 8, &save_task_handle);
#endif
}
