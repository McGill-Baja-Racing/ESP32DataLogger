#pragma once

// Private migration interface; not a public API. See src/README.md.
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
#include <atomic>

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

extern std::atomic<bool> s_sample_rpm_enabled;

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

extern QueueHandle_t twai_rx_raw_queue;
extern QueueHandle_t twai_rx_sample_queue;
extern QueueHandle_t twai_rx_servo_SDO_queue;
extern QueueHandle_t log_record_queue;




/* --------------------- Globals --------------------- */
extern twai_node_handle_t node_hdl;

extern twai_onchip_node_config_t node_cfg;
extern twai_event_callbacks_t cbs;

extern FILE *logFile;
extern int writeCounter;
extern std::atomic<bool> stop_logging;
extern SemaphoreHandle_t log_file_mutex;

/* Logger buffers + SD writer signalling */
#define SAMPLE_SIZE  11
#define SAMPLES_PER_BLOCK  100
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE * sizeof(uint32_t))

extern uint32_t logBuffer[SAMPLES_PER_BLOCK][SAMPLE_SIZE];

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
    int64_t received_us;
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

extern std::vector<ReplayData> data;
extern ecvt_fsm_t s_fsm;
extern ecvt_fsm_user_ctx_t s_fsm_ctx;

/* Latest CAN sensor values for use in calc_task and state logic. */
extern volatile uint32_t s_latest_throttle;
extern volatile float s_latest_engine_rpm;
extern volatile float s_latest_wheel_rpm;
extern volatile bool s_latest_brake_state;
extern volatile uint16_t s_latest_tpdo1_statusword;
extern volatile int64_t s_latest_tpdo1_rx_us;
extern volatile float s_target_engine_rpm_running_init;
extern volatile float s_target_engine_rpm_running;
extern volatile float s_lowest_gear_motor_pos;
extern volatile float s_highest_gear_motor_pos;
extern volatile uint32_t s_calc_moving_median_window_size;
extern volatile bool s_running_entry_needs_lowest_gear;
extern uint32_t s_stopped_to_brake_count;
extern uint32_t s_stopped_to_running_count;
extern uint32_t s_brake_release_count;
extern uint32_t s_running_to_brake_count;
extern uint32_t s_running_to_stopped_count;

