#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include <inttypes.h>
#include "esp_timer.h"
#include <string.h>

#include "driver/gpio.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <freertos/projdefs.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/*
 * Sensor node firmware map
 * ------------------------
 * This node listens for runtime configuration from the master, samples one or
 * more "virtual sensors", and transmits each sensor as its own CAN ID.
 *
 * Most common edit points:
 * - Physical board identity: NODE_ID.
 * - CAN pins/bitrate: TX_GPIO_NUM, RX_GPIO_NUM, TRANSM_RATE.
 * - Simulated sensors for testing: s_virtual_sensors[] and
 *   simulated_sensor_value().
 * - Real sensor tasks: ENABLE_ADC_SAMPLE_TASK / ENABLE_RPM_SAMPLE_TASK and the
 *   sample_adc_task()/sample_rpm_task() implementations.
 * - Health reporting: health_task() and send_node_health().
 *
 * Runtime data path:
 * master START/CONFIG -> receive_task() -> s_virtual_sensors[]
 * sample_task()/real sensor tasks -> sample_tx_queue -> send_task() -> CAN bus
 */

/* --------------------- Board identity and CAN config ------------------ */


#ifndef NODE_CAN_TX_GPIO
#define NODE_CAN_TX_GPIO        21              // CAN TX Pin
#endif

#ifndef NODE_CAN_RX_GPIO
#define NODE_CAN_RX_GPIO        20              // CAN RX Pin
#endif

#define TX_GPIO_NUM             NODE_CAN_TX_GPIO
#define RX_GPIO_NUM             NODE_CAN_RX_GPIO
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define TAG             "Node"
#define NODE_ID                 4               // CHANGE TO NODE ID
#define PULSES_PER_ROTATION     1               // Set this to the number of pulses in one full rotation
#define RPM_MAX_VALID           8000            // Reject faster pulses as noise/bounce
#define RPM_ZERO_TIMEOUT_US     5000000LL       // Report zero after no valid pulse for 5 seconds
#define RPM_GPIO_EDGE           GPIO_INTR_NEGEDGE // Conditioned tach pulse falls once per rotation
#define OLD_RPM_DETECT_THRESHOLD_MV 1500        // Legacy ADC RPM detects below this voltage
#define OLD_RPM_RESET_THRESHOLD_MV  1500        // Legacy ADC RPM re-arms above this voltage
#define BEARING_ENCODER_PULSES_PER_ROTATION 32  // Physical encoder cycles per shaft rotation
#define BEARING_ENCODER_COUNTS_PER_ROTATION (BEARING_ENCODER_PULSES_PER_ROTATION * 4)
#define BEARING_DEG_SCALE       10              // Bearing sample value is degrees x10

// -------------------- TASK RATE CONFIG --------------------
// Set SEND_PERIOD_MS to 0 to transmit queued samples as fast as TWAI accepts them.
#define SAMPLE_QUEUE_LENGTH     64
#define SIM_TASK_POLL_MS        1
#define SIM_TASK_IDLE_US        100
#define DEFAULT_SAMPLE_RATE_HZ  10
#define SAMPLE_TX_TIMEOUT_MS    0
#define CAN_TX_TIMEOUT_MS       50
#define CAN_RECOVERY_POLL_MS    250
#define ADC_SAMPLE_PERIOD_MS    1
#define RPM_SAMPLE_PERIOD_MS    2
#define SEND_PERIOD_MS          0
#define SERIAL_SAMPLE_LOG_PERIOD_MS 100

#ifndef NODE_SERIAL_TEST_MODE
#define NODE_SERIAL_TEST_MODE   0
#endif

#ifndef NODE_SERIAL_TEST_RPM_GPIO
#define NODE_SERIAL_TEST_RPM_GPIO 0
#endif

#ifndef NODE_SERIAL_TEST_ENCODER_ONLY
#define NODE_SERIAL_TEST_ENCODER_ONLY 0
#endif

#ifndef NODE_ENCODER_TEST_ONLY
#define NODE_ENCODER_TEST_ONLY 0
#endif

#ifndef NODE_SERIAL_DEBUG_SAMPLES
#define NODE_SERIAL_DEBUG_SAMPLES 0
#endif

#ifndef NODE_START_ACTIVE_ON_BOOT
#define NODE_START_ACTIVE_ON_BOOT 0
#endif

#ifndef NODE_SERIAL_TEST_ENCODER_GPIO_A
#define NODE_SERIAL_TEST_ENCODER_GPIO_A 6
#endif

#ifndef NODE_SERIAL_TEST_ENCODER_GPIO_B
#define NODE_SERIAL_TEST_ENCODER_GPIO_B 7
#endif

#define NODE_SERIAL_ENCODER_TEST (NODE_SERIAL_TEST_MODE && NODE_SERIAL_TEST_ENCODER_ONLY)
#define NODE_ENCODER_ONLY_TEST (NODE_ENCODER_TEST_ONLY || NODE_SERIAL_ENCODER_TEST)

#define ENABLE_SIMULATED_SENSOR_TASK 1
#define ENABLE_ADC_SAMPLE_TASK     0
#define ENABLE_RPM_SAMPLE_TASK     0
// ID1 = brakes
// ID2 = wheel
// ID0 = engine

/* --------------------- Shared master/node CAN protocol ------------------ */

#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_MASTER_TIME_BEACON   0x0A2
#define ID_NODE_CONFIG_CMD      0x0A3
#define ID_NODE_STATE           (0x0C0 + NODE_ID)
#define ID_NODE_HEALTH          (0x180 + NODE_ID)
#define NODE_STATE_LOW_POWER    0
#define NODE_STATE_ACTIVE       1
#define NODE_STATE_REASON_BOOT  1
#define NODE_STATE_REASON_STOP  2
#define NODE_STATE_REASON_START 3
#define NODE_STATE_REASON_RECOVERY 4
#define NODE_CONFIG_CMD_RESET   0
#define NODE_CONFIG_CMD_SENSOR  1
#define NODE_CONFIG_CMD_LOG     2
#define NODE_CONFIG_CMD_SENSOR_IO 3
#define NODE_CONFIG_SENSOR_ENABLED 0x01
#define NODE_LOG_MODE_STATUS    0x01
#define NODE_LOG_MODE_SAMPLES   0x02
#define NODE_SENSOR_FUNCTION_SIM 0
#define NODE_SENSOR_FUNCTION_ADC 1
#define NODE_SENSOR_FUNCTION_RPM 2
#define NODE_SENSOR_FUNCTION_FRONT_BRAKE 3
#define NODE_SENSOR_FUNCTION_REAR_BRAKE 4
#define NODE_SENSOR_FUNCTION_OLD_RPM 5
#define NODE_SENSOR_FUNCTION_BEARING 6
#define NODE_SENSOR_FUNCTION_ACCEL_X 7
#define NODE_SENSOR_FUNCTION_ACCEL_Y 8
#define NODE_SENSOR_FUNCTION_ACCEL_Z 9

#if NODE_SERIAL_TEST_MODE
#define DEFAULT_WHEEL_RPM_FUNCTION NODE_SENSOR_FUNCTION_OLD_RPM
#else
#define DEFAULT_WHEEL_RPM_FUNCTION NODE_SENSOR_FUNCTION_RPM
#endif

#define ID_SENSOR_STEERING      (0x0B1 + NODE_ID)   // 0x0B3 for node 2
#define ID_SENSOR_BRAKE         (ID_SENSOR_STEERING + 1)
#define ID_SENSOR_WHEEL_RPM     (ID_SENSOR_STEERING + 2)
#define ID_SENSOR_BEARING       0x0D08
#define ID_SENSOR_ACCEL_X       0x0D11
#define ID_SENSOR_ACCEL_Y       0x0D12
#define ID_SENSOR_ACCEL_Z       0x0D13
#define NODE_HEALTH_PERIOD_MS   500
#define MAX_VIRTUAL_SENSORS     8

/* --------------------- Local data structures and state ------------------ */

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

typedef struct {
    uint32_t can_id;
    int32_t timestamp_ms;
    int32_t value;
} sample_data_t;

typedef struct {
    uint8_t sensor_id;
    const char *name;
    uint32_t can_id;
    bool enabled;
    uint32_t sample_rate_hz;
    uint32_t period_us;
    int64_t next_sample_us;
    uint8_t function;
    uint8_t port;
    uint8_t aux_port;
    bool rpm_peak_detected;
    int64_t rpm_last_pulse_us;
    int32_t rpm_last_value;
    volatile int32_t encoder_count;
    uint8_t encoder_last_state;
} virtual_sensor_t;

static QueueHandle_t twai_rx_queue;
static QueueHandle_t sample_tx_queue;
static volatile bool s_node_active = false;
static uint8_t s_boot_reset_reason = ESP_RST_UNKNOWN;
static volatile uint32_t s_sample_loop_busy_us = 0;
static volatile uint32_t s_sample_loop_max_us = 0;
static volatile uint32_t s_send_task_busy_us = 0;
static volatile uint32_t s_sample_lateness_max_us = 0;
static volatile uint32_t s_missed_deadlines = 0;
static volatile uint32_t s_sample_drop_count = 0;
static volatile uint32_t s_tx_fail_count = 0;
static volatile uint8_t s_log_mode = 0;
static portMUX_TYPE s_rpm_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_encoder_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_gpio_isr_service_installed = false;
static virtual_sensor_t s_virtual_sensors[MAX_VIRTUAL_SENSORS] = {
    {22, "wheel_rpm", ID_SENSOR_WHEEL_RPM, !NODE_ENCODER_ONLY_TEST, 500, 2000, 0, DEFAULT_WHEEL_RPM_FUNCTION, NODE_SERIAL_TEST_RPM_GPIO, 0, false, 0, 0, 0, 0},
    {20, "steering_angle", ID_SENSOR_STEERING, false, DEFAULT_SAMPLE_RATE_HZ, 100000, 0, NODE_SENSOR_FUNCTION_SIM, 0, 0, false, 0, 0, 0, 0},
    {21, "brake_pressure", ID_SENSOR_BRAKE, false, DEFAULT_SAMPLE_RATE_HZ, 100000, 0, NODE_SENSOR_FUNCTION_SIM, 0, 0, false, 0, 0, 0, 0},
    {30, "accel_x", ID_SENSOR_ACCEL_X, !NODE_ENCODER_ONLY_TEST, 50, 20000, 0, NODE_SENSOR_FUNCTION_ACCEL_X, 1, 0, false, 0, 0, 0, 0},
    {31, "accel_y", ID_SENSOR_ACCEL_Y, !NODE_ENCODER_ONLY_TEST, 50, 20000, 0, NODE_SENSOR_FUNCTION_ACCEL_Y, 2, 0, false, 0, 0, 0, 0},
    {32, "accel_z", ID_SENSOR_ACCEL_Z, !NODE_ENCODER_ONLY_TEST, 50, 20000, 0, NODE_SENSOR_FUNCTION_ACCEL_Z, 3, 0, false, 0, 0, 0, 0},
    {33, "bearing_encoder", ID_SENSOR_BEARING, NODE_ENCODER_ONLY_TEST, 50, 20000, 0, NODE_SENSOR_FUNCTION_BEARING, NODE_SERIAL_TEST_ENCODER_GPIO_A, NODE_SERIAL_TEST_ENCODER_GPIO_B, false, 0, 0, 0, 0},
};

/* --------------------- Optional ADC configuration ------------------ */

// -------------------- ADC CONFIG (CHANGE THESE) --------------------
// Pick the ADC unit + channel that corresponds to your chosen GPIO.
// ESP32-C3 ADC1 channels map to GPIO0-GPIO4.
static const adc_channel_t ADC_CH_USED   = ADC_CHANNEL_0;
static const adc_atten_t   ADC_ATTEN_USED = ADC_ATTEN_DB_12;
static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static adc_cali_handle_t s_adc_cali_handles[5] = {};
static bool s_adc_channel_configured[5] = {};

typedef struct {
    float min_sensor_voltage_mv;
    float max_sensor_voltage_mv;
    float pressure_span_psi;
    const char *caption;
} brake_pressure_config_t;

#define BRAKE_SENSOR_DIVIDER_SCALE (2.33f / 4.33f)
#define ADXL377_SENSITIVITY_MV_PER_G 6.5f

static const brake_pressure_config_t FRONT_BRAKE_CONFIG = {
    .min_sensor_voltage_mv = 500.0f,
    .max_sensor_voltage_mv = 4500.0f,
    .pressure_span_psi = 3000.0f,
    .caption = "front_brake",
};

static const brake_pressure_config_t REAR_BRAKE_CONFIG = {
    .min_sensor_voltage_mv = 500.0f,
    .max_sensor_voltage_mv = 4500.0f,
    .pressure_span_psi = 1600.0f,
    .caption = "rear_brake",
};

typedef struct {
    float zero_g_voltage_mv;
    const char *caption;
} acceleration_axis_config_t;

static const acceleration_axis_config_t ACCEL_X_CONFIG = {
    .zero_g_voltage_mv = 1677.0f,
    .caption = "accel_x",
};

static const acceleration_axis_config_t ACCEL_Y_CONFIG = {
    .zero_g_voltage_mv = 2329.0f,
    .caption = "accel_y",
};

static const acceleration_axis_config_t ACCEL_Z_CONFIG = {
    .zero_g_voltage_mv = 1687.0f,
    .caption = "accel_z",
};

/* --------------------- Hardware callbacks and time sync ------------------ */

static bool adc_calibration_init_channel(adc_channel_t channel)
{
    if (channel < ADC_CHANNEL_0 || channel > ADC_CHANNEL_4) {
        return false;
    }
    if (s_adc_cali_handles[channel]) {
        return true;
    }

    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    bool cali_enable = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = channel,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
#endif

    if (ret == ESP_OK) {
        s_adc_cali_handles[channel] = handle;
        cali_enable = true;
        ESP_LOGI(TAG, "ADC calibration enabled for GPIO/channel %d", channel);
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "ADC calibration unavailable for GPIO/channel %d", channel);
    } else {
        ESP_LOGW(TAG,
                 "ADC calibration failed for GPIO/channel %d: %s",
                 channel,
                 esp_err_to_name(ret));
    }

    return cali_enable;
}

static esp_err_t adc_oneshot_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    return adc_oneshot_new_unit(&init_config, &s_adc1_handle);
}

static esp_err_t adc_read_raw(adc_channel_t channel, int *raw)
{
    if (!s_adc1_handle || !raw) {
        return ESP_ERR_INVALID_STATE;
    }
    return adc_oneshot_read(s_adc1_handle, channel, raw);
}

static esp_err_t adc_read_voltage_mv(adc_channel_t channel, int *voltage_mv)
{
    int raw = 0;
    esp_err_t err = adc_read_raw(channel, &raw);
    if (err != ESP_OK) {
        return err;
    }

    adc_cali_handle_t cali = s_adc_cali_handles[channel];
    if (cali) {
        return adc_cali_raw_to_voltage(cali, raw, voltage_mv);
    }

    *voltage_mv = (raw * 2450) / 4095;
    return ESP_OK;
}

static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8];
    twai_frame_t frame = {
        .buffer     = raw,
        .buffer_len = sizeof(raw),
    };

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {

        can_rx_word_t msg;
        msg.id  = frame.header.id;
        msg.dlc = frame.header.dlc;
        msg.data = 0;

        // Pack into uint64_t (little-endian)
        for (int i = 0; i < msg.dlc; i++) {
            msg.data |= ((uint64_t)raw[i] << (8 * i));
        }

        xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

/* --------------------------- New Library Functions -------------------------- */



static uint8_t ping_data[1];
static uint8_t start_data[1];
static uint8_t stop_data[1];


twai_node_handle_t node_hdl = NULL;
twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,             // TWAI TX GPIO pin
    .io_cfg.rx = RX_GPIO_NUM,             // TWAI RX GPIO pin
    .bit_timing.bitrate = TRANSM_RATE,    // bps bitrate
    .tx_queue_depth = TX_QUEUE_DEPTH,     // Transmit queue depth set to 5
};

static const twai_frame_t start_message = {
    .header.id = ID_START_CMD,    // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = start_data,            // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

static const twai_frame_t stop_message = {
    .header.id = ID_STOP_CMD,     // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = stop_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

// Receive from CAN ISR handler
twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

static volatile int64_t g_offset_us;

static inline void handle_time_beacon(const can_rx_word_t *rx){
    uint64_t t_main_us = rx ->data;
    int64_t t_local_us = esp_timer_get_time();

    g_offset_us = (int64_t)t_main_us - t_local_us;
}

static inline int32_t get_synced_timestamp_ms(void)
{
    int64_t t_local_sample_us = esp_timer_get_time();
    int64_t t_main_sample_us = t_local_sample_us + g_offset_us;

    return (int32_t)(t_main_sample_us / 1000);
}

/* --------------------- Small utility helpers ------------------ */

static inline uint8_t rx_byte(const can_rx_word_t *rx, uint8_t index)
{
    return (uint8_t)((rx->data >> (8 * index)) & 0xFFu);
}

static TickType_t ms_to_ticks_at_least_one(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static uint8_t u32_to_u8_saturating(uint32_t value)
{
    return value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
}

static void record_sample_loop_busy_time(uint32_t busy_us)
{
    uint32_t total = s_sample_loop_busy_us + busy_us;
    s_sample_loop_busy_us = total < s_sample_loop_busy_us ? UINT32_MAX : total;

    if (busy_us > s_sample_loop_max_us) {
        s_sample_loop_max_us = busy_us;
    }
}

static void record_send_task_busy_time(uint32_t busy_us)
{
    uint32_t total = s_send_task_busy_us + busy_us;
    s_send_task_busy_us = total < s_send_task_busy_us ? UINT32_MAX : total;
}

static void record_sample_lateness_us(uint32_t lateness_us)
{
    if (lateness_us > s_sample_lateness_max_us) {
        s_sample_lateness_max_us = lateness_us;
    }
    if (lateness_us > 1000u) {
        s_missed_deadlines++;
    }
}

static bool rx_targets_this_node(const can_rx_word_t *rx)
{
    if (rx->dlc == 0) {
        return true;
    }

    uint8_t target_node_id = rx_byte(rx, 0);
    return target_node_id == NODE_ID || target_node_id == 0xFF;
}

static bool rx_is_broadcast_command(const can_rx_word_t *rx)
{
    return rx->dlc == 0 || rx_byte(rx, 0) == 0xFF;
}

static bool node_log_status_enabled(void)
{
    return (s_log_mode & NODE_LOG_MODE_STATUS) != 0;
}

static bool node_log_samples_enabled(void)
{
    return (s_log_mode & NODE_LOG_MODE_SAMPLES) != 0;
}

/* --------------------- Runtime sensor configuration ------------------ */

static uint32_t sample_rate_to_period_us(uint16_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
    }

    uint32_t period_us = (1000000u + (sample_rate_hz / 2u)) / sample_rate_hz;
    if (period_us < 1u) {
        period_us = 1u;
    }

    return period_us;
}

static virtual_sensor_t *find_virtual_sensor(uint8_t sensor_id)
{
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        if (s_virtual_sensors[i].sensor_id == sensor_id) {
            return &s_virtual_sensors[i];
        }
    }

    return NULL;
}

static virtual_sensor_t *find_or_allocate_virtual_sensor(uint8_t sensor_id)
{
    virtual_sensor_t *sensor = find_virtual_sensor(sensor_id);
    if (sensor) {
        return sensor;
    }

    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        if (s_virtual_sensors[i].sensor_id == 0) {
            s_virtual_sensors[i].sensor_id = sensor_id;
            s_virtual_sensors[i].name = "runtime_sensor";
            s_virtual_sensors[i].can_id = ID_SENSOR_STEERING + sensor_id;
            s_virtual_sensors[i].enabled = false;
            s_virtual_sensors[i].sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
            s_virtual_sensors[i].period_us = sample_rate_to_period_us(DEFAULT_SAMPLE_RATE_HZ);
            s_virtual_sensors[i].next_sample_us = 0;
            s_virtual_sensors[i].function = NODE_SENSOR_FUNCTION_SIM;
            s_virtual_sensors[i].port = 0;
            s_virtual_sensors[i].aux_port = 0;
            s_virtual_sensors[i].rpm_peak_detected = false;
            s_virtual_sensors[i].rpm_last_pulse_us = 0;
            s_virtual_sensors[i].rpm_last_value = 0;
            s_virtual_sensors[i].encoder_count = 0;
            s_virtual_sensors[i].encoder_last_state = 0;
            return &s_virtual_sensors[i];
        }
    }

    return NULL;
}

static const char *sensor_function_name(uint8_t function)
{
    switch (function) {
    case NODE_SENSOR_FUNCTION_ADC:
        return "adc";
    case NODE_SENSOR_FUNCTION_RPM:
        return "rpm";
    case NODE_SENSOR_FUNCTION_OLD_RPM:
        return "old_rpm";
    case NODE_SENSOR_FUNCTION_FRONT_BRAKE:
        return "front_brake";
    case NODE_SENSOR_FUNCTION_REAR_BRAKE:
        return "rear_brake";
    case NODE_SENSOR_FUNCTION_BEARING:
        return "bearing";
    case NODE_SENSOR_FUNCTION_ACCEL_X:
        return "accel_x";
    case NODE_SENSOR_FUNCTION_ACCEL_Y:
        return "accel_y";
    case NODE_SENSOR_FUNCTION_ACCEL_Z:
        return "accel_z";
    case NODE_SENSOR_FUNCTION_SIM:
    default:
        return "sim";
    }
}

static bool gpio_to_adc_channel(uint8_t gpio, adc_channel_t *out_channel)
{
    if (!out_channel || gpio > 4) {
        return false;
    }

    *out_channel = (adc_channel_t)gpio;
    return true;
}

static bool configure_sensor_adc_port(virtual_sensor_t *sensor)
{
    adc_channel_t channel;
    if (!gpio_to_adc_channel(sensor->port, &channel)) {
        ESP_LOGW(TAG,
                 "Sensor %u uses %s on unsupported ADC GPIO%u; ESP32-C3 ADC1 supports GPIO0-GPIO4",
                 sensor->sensor_id,
                 sensor_function_name(sensor->function),
                 sensor->port);
        return false;
    }

    if (s_adc_channel_configured[channel]) {
        return true;
    }

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc1_handle, channel, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to configure ADC GPIO%u channel=%d err=%s",
                 sensor->port,
                 channel,
                 esp_err_to_name(err));
        return false;
    }

    s_adc_channel_configured[channel] = true;
    (void)adc_calibration_init_channel(channel);
    ESP_LOGI(TAG,
             "Configured calibrated ADC oneshot GPIO%u channel=%d attenuation=%d",
             sensor->port,
             channel,
             ADC_ATTEN_USED);
    return true;
}

static bool gpio_port_is_valid(uint8_t gpio)
{
    return gpio < GPIO_NUM_MAX;
}

static bool ensure_gpio_isr_service_installed(void)
{
    if (s_gpio_isr_service_installed) {
        return true;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
        return false;
    }
    s_gpio_isr_service_installed = true;
    return true;
}

static void IRAM_ATTR rpm_gpio_isr_handler(void *arg)
{
    virtual_sensor_t *sensor = (virtual_sensor_t *)arg;
    if (!sensor || !s_node_active) {
        return;
    }

    int64_t now_us = esp_timer_get_time() + g_offset_us;
    const int64_t minimum_pulse_interval_us =
        60000000LL / (RPM_MAX_VALID * PULSES_PER_ROTATION);

    portENTER_CRITICAL_ISR(&s_rpm_lock);
    if (sensor->rpm_last_pulse_us != 0) {
        int64_t delta_us = now_us - sensor->rpm_last_pulse_us;
        if (delta_us >= minimum_pulse_interval_us) {
            sensor->rpm_last_value =
                (int32_t)(60000000LL / (delta_us * PULSES_PER_ROTATION));
            sensor->rpm_last_pulse_us = now_us;
        }
    } else {
        sensor->rpm_last_pulse_us = now_us;
    }
    portEXIT_CRITICAL_ISR(&s_rpm_lock);
}

static bool configure_sensor_rpm_gpio_port(virtual_sensor_t *sensor)
{
    if (!sensor || !gpio_port_is_valid(sensor->port)) {
        ESP_LOGW(TAG,
                 "Sensor %u uses rpm on unsupported GPIO%u",
                 sensor ? sensor->sensor_id : 0,
                 sensor ? sensor->port : 0);
        return false;
    }

    if (!ensure_gpio_isr_service_installed()) {
        return false;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << sensor->port,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = RPM_GPIO_EDGE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to configure RPM GPIO%u: %s",
                 sensor->port,
                 esp_err_to_name(err));
        return false;
    }

    (void)gpio_isr_handler_remove((gpio_num_t)sensor->port);
    err = gpio_isr_handler_add((gpio_num_t)sensor->port, rpm_gpio_isr_handler, sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to add RPM ISR GPIO%u: %s",
                 sensor->port,
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "Configured RPM interrupt sensor=%u GPIO%u edge=%s",
             sensor->sensor_id,
             sensor->port,
             RPM_GPIO_EDGE == GPIO_INTR_NEGEDGE ? "falling" : "rising");
    return true;
}

static const int8_t s_encoder_states[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0,
};

static uint8_t read_encoder_state(const virtual_sensor_t *sensor)
{
    uint8_t a = gpio_get_level((gpio_num_t)sensor->port) ? 1u : 0u;
    uint8_t b = gpio_get_level((gpio_num_t)sensor->aux_port) ? 1u : 0u;
    return (uint8_t)((a << 1) | b);
}

static void IRAM_ATTR bearing_encoder_isr_handler(void *arg)
{
    virtual_sensor_t *sensor = (virtual_sensor_t *)arg;
    if (!sensor) {
        return;
    }

    uint8_t new_state = read_encoder_state(sensor);

    portENTER_CRITICAL_ISR(&s_encoder_lock);
    uint8_t index = (uint8_t)(((sensor->encoder_last_state << 2) | new_state) & 0x0F);
    sensor->encoder_count += s_encoder_states[index];
    sensor->encoder_last_state = new_state;
    portEXIT_CRITICAL_ISR(&s_encoder_lock);
}

static void remove_bearing_encoder_handlers(const virtual_sensor_t *sensor)
{
    if (!sensor) {
        return;
    }
    if (gpio_port_is_valid(sensor->port)) {
        (void)gpio_isr_handler_remove((gpio_num_t)sensor->port);
    }
    if (gpio_port_is_valid(sensor->aux_port) && sensor->aux_port != sensor->port) {
        (void)gpio_isr_handler_remove((gpio_num_t)sensor->aux_port);
    }
}

static bool configure_sensor_bearing_encoder_ports(virtual_sensor_t *sensor)
{
    if (!sensor ||
        !gpio_port_is_valid(sensor->port) ||
        !gpio_port_is_valid(sensor->aux_port) ||
        sensor->port == sensor->aux_port) {
        ESP_LOGW(TAG,
                 "Sensor %u uses bearing encoder on unsupported GPIO%u/GPIO%u",
                 sensor ? sensor->sensor_id : 0,
                 sensor ? sensor->port : 0,
                 sensor ? sensor->aux_port : 0);
        return false;
    }

    if (!ensure_gpio_isr_service_installed()) {
        return false;
    }

    gpio_config_t config = {
        .pin_bit_mask = (1ULL << sensor->port) | (1ULL << sensor->aux_port),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to configure bearing encoder GPIO%u/GPIO%u: %s",
                 sensor->port,
                 sensor->aux_port,
                 esp_err_to_name(err));
        return false;
    }

    remove_bearing_encoder_handlers(sensor);

    portENTER_CRITICAL(&s_encoder_lock);
    sensor->encoder_count = 0;
    sensor->encoder_last_state = read_encoder_state(sensor);
    portEXIT_CRITICAL(&s_encoder_lock);

    err = gpio_isr_handler_add((gpio_num_t)sensor->port,
                               bearing_encoder_isr_handler,
                               sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to add bearing encoder ISR GPIO%u: %s",
                 sensor->port,
                 esp_err_to_name(err));
        return false;
    }

    err = gpio_isr_handler_add((gpio_num_t)sensor->aux_port,
                               bearing_encoder_isr_handler,
                               sensor);
    if (err != ESP_OK) {
        (void)gpio_isr_handler_remove((gpio_num_t)sensor->port);
        ESP_LOGW(TAG,
                 "Failed to add bearing encoder ISR GPIO%u: %s",
                 sensor->aux_port,
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "Configured bearing encoder sensor=%u GPIOA=%u GPIOB=%u counts/rev=%u",
             sensor->sensor_id,
             sensor->port,
             sensor->aux_port,
             BEARING_ENCODER_COUNTS_PER_ROTATION);
    return true;
}

static bool sensor_is_default_accelerometer(const virtual_sensor_t *sensor)
{
    if (!sensor) {
        return false;
    }

    return sensor->sensor_id == 30 ||
           sensor->sensor_id == 31 ||
           sensor->sensor_id == 32;
}

static void configure_enabled_adc_sensors(void);

static void reset_runtime_sensor_config(void)
{
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        if (s_virtual_sensors[i].function == NODE_SENSOR_FUNCTION_RPM &&
            gpio_port_is_valid(s_virtual_sensors[i].port)) {
            (void)gpio_isr_handler_remove((gpio_num_t)s_virtual_sensors[i].port);
        } else if (s_virtual_sensors[i].function == NODE_SENSOR_FUNCTION_BEARING) {
            remove_bearing_encoder_handlers(&s_virtual_sensors[i]);
        }
        bool enable_by_default = sensor_is_default_accelerometer(&s_virtual_sensors[i]);
        uint32_t sample_rate_hz = enable_by_default ? 50 : DEFAULT_SAMPLE_RATE_HZ;

        s_virtual_sensors[i].enabled = enable_by_default;
        s_virtual_sensors[i].sample_rate_hz = sample_rate_hz;
        s_virtual_sensors[i].period_us = sample_rate_to_period_us(sample_rate_hz);
        s_virtual_sensors[i].next_sample_us = 0;
        s_virtual_sensors[i].rpm_peak_detected = false;
        s_virtual_sensors[i].rpm_last_pulse_us = 0;
        s_virtual_sensors[i].rpm_last_value = 0;
        s_virtual_sensors[i].encoder_count = 0;
        s_virtual_sensors[i].encoder_last_state = 0;
    }

    configure_enabled_adc_sensors();
    ESP_LOGI(TAG, "Runtime sensor config reset; default accelerometer sensors enabled");
}

static void arm_sensor_schedule(void)
{
    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        s_virtual_sensors[i].next_sample_us = now_us;
    }
}

static void configure_enabled_adc_sensors(void)
{
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        virtual_sensor_t *sensor = &s_virtual_sensors[i];
        if (sensor->sensor_id == 0 || !sensor->enabled) {
            continue;
        }

        if (sensor->function == NODE_SENSOR_FUNCTION_ADC ||
            sensor->function == NODE_SENSOR_FUNCTION_OLD_RPM ||
            sensor->function == NODE_SENSOR_FUNCTION_FRONT_BRAKE ||
            sensor->function == NODE_SENSOR_FUNCTION_REAR_BRAKE ||
            sensor->function == NODE_SENSOR_FUNCTION_ACCEL_X ||
            sensor->function == NODE_SENSOR_FUNCTION_ACCEL_Y ||
            sensor->function == NODE_SENSOR_FUNCTION_ACCEL_Z) {
            (void)configure_sensor_adc_port(sensor);
        }
    }
}

static void configure_enabled_rpm_sensors(void)
{
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        virtual_sensor_t *sensor = &s_virtual_sensors[i];
        if (sensor->sensor_id == 0 || !sensor->enabled) {
            continue;
        }

        if (sensor->function == NODE_SENSOR_FUNCTION_RPM) {
            (void)configure_sensor_rpm_gpio_port(sensor);
        }
    }
}

static void configure_enabled_bearing_sensors(void)
{
    for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
        virtual_sensor_t *sensor = &s_virtual_sensors[i];
        if (sensor->sensor_id == 0 || !sensor->enabled) {
            continue;
        }

        if (sensor->function == NODE_SENSOR_FUNCTION_BEARING) {
            (void)configure_sensor_bearing_encoder_ports(sensor);
        }
    }
}

static void apply_sensor_runtime_config(const can_rx_word_t *rx)
{
    if (rx->dlc < 8 || !rx_targets_this_node(rx)) {
        return;
    }

    uint8_t sensor_id = rx_byte(rx, 2);
    uint8_t flags = rx_byte(rx, 3);
    uint16_t sample_rate_hz = (uint16_t)rx_byte(rx, 4) |
                          ((uint16_t)rx_byte(rx, 5) << 8);
    uint16_t can_id = (uint16_t)rx_byte(rx, 6) |
                  ((uint16_t)rx_byte(rx, 7) << 8);

    virtual_sensor_t *sensor = find_or_allocate_virtual_sensor(sensor_id);
    if (!sensor) {
        ESP_LOGW(TAG, "Ignoring config for sensor_id=%u; no free runtime sensor slots", sensor_id);
        return;
    }

    sensor->enabled = (flags & NODE_CONFIG_SENSOR_ENABLED) != 0;
    sensor->sample_rate_hz = sample_rate_hz == 0 ? DEFAULT_SAMPLE_RATE_HZ : sample_rate_hz;
    sensor->period_us = sample_rate_to_period_us(sample_rate_hz);
    sensor->can_id = can_id;
    sensor->next_sample_us = 0;

    ESP_LOGI(TAG,
             "CONFIG sensor=%u %s enabled=%s can_id=0x%03" PRIX32
             " rate=%" PRIu32 "Hz period=%" PRIu32 "us",
             sensor->sensor_id,
             sensor->name,
             sensor->enabled ? "true" : "false",
             sensor->can_id,
             sensor->sample_rate_hz,
	             sensor->period_us);
}

static void apply_sensor_io_runtime_config(const can_rx_word_t *rx)
{
    if (rx->dlc < 8 || !rx_targets_this_node(rx)) {
        return;
    }

    uint8_t sensor_id = rx_byte(rx, 2);
    uint8_t function = rx_byte(rx, 3);
    uint8_t port = rx_byte(rx, 4);
    uint8_t aux_port = rx_byte(rx, 5);

    virtual_sensor_t *sensor = find_or_allocate_virtual_sensor(sensor_id);
    if (!sensor) {
        ESP_LOGW(TAG, "Ignoring IO config for sensor_id=%u; no free runtime sensor slots", sensor_id);
        return;
    }

    if (function > NODE_SENSOR_FUNCTION_ACCEL_Z) {
        function = NODE_SENSOR_FUNCTION_SIM;
    }

    uint8_t old_function = sensor->function;
    uint8_t old_port = sensor->port;
    uint8_t old_aux_port = sensor->aux_port;
    if (old_function == NODE_SENSOR_FUNCTION_RPM && gpio_port_is_valid(old_port)) {
        (void)gpio_isr_handler_remove((gpio_num_t)old_port);
    } else if (old_function == NODE_SENSOR_FUNCTION_BEARING) {
        virtual_sensor_t old_sensor = *sensor;
        old_sensor.port = old_port;
        old_sensor.aux_port = old_aux_port;
        remove_bearing_encoder_handlers(&old_sensor);
    }

    sensor->function = function;
    sensor->port = port;
    sensor->aux_port = aux_port;
    sensor->rpm_peak_detected = false;
    sensor->rpm_last_pulse_us = 0;
    sensor->rpm_last_value = 0;
    sensor->encoder_count = 0;
    sensor->encoder_last_state = 0;

    if (sensor->function == NODE_SENSOR_FUNCTION_RPM) {
        (void)configure_sensor_rpm_gpio_port(sensor);
    } else if (sensor->function == NODE_SENSOR_FUNCTION_BEARING) {
        (void)configure_sensor_bearing_encoder_ports(sensor);
    } else if (sensor->function == NODE_SENSOR_FUNCTION_ADC ||
        sensor->function == NODE_SENSOR_FUNCTION_OLD_RPM ||
        sensor->function == NODE_SENSOR_FUNCTION_FRONT_BRAKE ||
        sensor->function == NODE_SENSOR_FUNCTION_REAR_BRAKE ||
        sensor->function == NODE_SENSOR_FUNCTION_ACCEL_X ||
        sensor->function == NODE_SENSOR_FUNCTION_ACCEL_Y ||
        sensor->function == NODE_SENSOR_FUNCTION_ACCEL_Z) {
        (void)configure_sensor_adc_port(sensor);
    }

    ESP_LOGI(TAG,
             "CONFIG IO sensor=%u function=%s port=GPIO%u aux=GPIO%u",
             sensor->sensor_id,
             sensor_function_name(sensor->function),
             sensor->port,
             sensor->aux_port);
}

static void apply_log_runtime_config(const can_rx_word_t *rx)
{
    if (rx->dlc < 3 || !rx_targets_this_node(rx)) {
        return;
    }

    s_log_mode = rx_byte(rx, 2) & (NODE_LOG_MODE_STATUS | NODE_LOG_MODE_SAMPLES);
    ESP_LOGI(TAG,
             "LOG mode updated: status=%s samples=%s",
             node_log_status_enabled() ? "on" : "off",
             node_log_samples_enabled() ? "on" : "off");
}

static void handle_node_config_frame(const can_rx_word_t *rx)
{
    if (rx->dlc < 2 || !rx_targets_this_node(rx)) {
        return;
    }

    uint8_t command = rx_byte(rx, 1);
    if (command == NODE_CONFIG_CMD_RESET) {
        reset_runtime_sensor_config();
    } else if (command == NODE_CONFIG_CMD_SENSOR) {
        apply_sensor_runtime_config(rx);
    } else if (command == NODE_CONFIG_CMD_LOG) {
        apply_log_runtime_config(rx);
    } else if (command == NODE_CONFIG_CMD_SENSOR_IO) {
        apply_sensor_io_runtime_config(rx);
    } else {
        ESP_LOGW(TAG, "Unknown node config command=%u", command);
    }
}

/* --------------------- Sample queue and node status transmitters ------------------ */

static bool queue_sample(uint32_t can_id, int32_t timestamp_ms, int32_t value)
{
    if (!s_node_active) {
        return false;
    }

    sample_data_t sample = {
        .can_id = can_id,
        .timestamp_ms = timestamp_ms,
        .value = value,
    };

    if (xQueueSend(sample_tx_queue, &sample, pdMS_TO_TICKS(SAMPLE_TX_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }

    sample_data_t dropped;
    (void)xQueueReceive(sample_tx_queue, &dropped, 0);
    if (xQueueSend(sample_tx_queue, &sample, 0) != pdTRUE) {
        s_sample_drop_count++;
        if ((s_sample_drop_count % 100u) == 1u) {
            ESP_LOGW(TAG,
                     "Sample TX queue full - newest dropped | id=0x%03" PRIX32
                     " ts=%" PRIi32 " val=%" PRIi32 " drops=%" PRIu32,
                     can_id,
                     timestamp_ms,
                     value,
                     s_sample_drop_count);
        }
        return false;
    }

    s_sample_drop_count++;
    if ((s_sample_drop_count % 100u) == 1u) {
        ESP_LOGW(TAG,
                 "Sample TX queue full - dropped oldest id=0x%03" PRIX32
                 " drops=%" PRIu32,
                 dropped.can_id,
                 s_sample_drop_count);
    }
    return true;
}

static uint64_t pack_sample_data(const sample_data_t *sample)
{
    return ((uint64_t)(uint32_t)sample->timestamp_ms << 32)
         | ((uint64_t)(uint32_t)sample->value);
}

static const char *node_state_reason_name(uint8_t reason)
{
    switch (reason) {
    case NODE_STATE_REASON_BOOT:
        return "boot";
    case NODE_STATE_REASON_STOP:
        return "stop";
    case NODE_STATE_REASON_START:
        return "start";
    case NODE_STATE_REASON_RECOVERY:
        return "recovery";
    default:
        return "unknown";
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        return "unknown";
    }
}

static void send_node_state(uint8_t state, uint8_t reason)
{
    uint8_t raw[3] = {state, reason, reason == NODE_STATE_REASON_BOOT ? s_boot_reset_reason : 0};
    twai_frame_t tx = {
        .header.id = ID_NODE_STATE,
        .header.ide = false,
        .buffer = raw,
        .buffer_len = sizeof(raw),
    };

    esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TX STATE | node=%d state=%s reason=%s reset=%s(%u)",
                 NODE_ID,
                 state == NODE_STATE_ACTIVE ? "active" : "low-power",
                 node_state_reason_name(reason),
                 reset_reason_name((esp_reset_reason_t)s_boot_reset_reason),
                 s_boot_reset_reason);
    } else {
        ESP_LOGW(TAG, "TX STATE failed | node=%d state=%u reason=%u err=%s",
                 NODE_ID, state, reason, esp_err_to_name(err));
    }
}

static void send_node_health(uint8_t load_percent,
                             uint8_t max_lateness_ms,
                             uint8_t sample_queue_depth,
                             uint8_t missed_deadlines,
                             uint8_t tx_fail_count,
                             uint8_t free_heap_kb)
{
    uint8_t flags = 0;
    if (s_node_active) {
        flags |= 0x01u;
    }
    if (s_sample_drop_count > 0) {
        flags |= 0x02u;
    }

    uint8_t raw[8] = {
        NODE_ID,
        flags,
        load_percent,
        max_lateness_ms,
        sample_queue_depth,
        missed_deadlines,
        tx_fail_count,
        free_heap_kb,
    };

    twai_frame_t tx = {
        .header.id = ID_NODE_HEALTH,
        .header.ide = false,
        .buffer = raw,
        .buffer_len = sizeof(raw),
    };

    esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(CAN_TX_TIMEOUT_MS));
    if (err != ESP_OK) {
        s_tx_fail_count++;
        ESP_LOGW(TAG, "TX HEALTH failed | node=%d err=%s", NODE_ID, esp_err_to_name(err));
    }
}

/* --------------------- Health task ------------------ */

static void health_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_report_us = esp_timer_get_time();
    uint32_t report_count = 0;

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(NODE_HEALTH_PERIOD_MS));

        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed_us = (uint32_t)(now_us - last_report_us);
        last_report_us = now_us;

        uint32_t busy_us = s_sample_loop_busy_us + s_send_task_busy_us;
        uint32_t max_lateness_us = s_sample_lateness_max_us;
        uint32_t missed_deadlines = s_missed_deadlines;

        s_sample_loop_busy_us = 0;
        s_sample_loop_max_us = 0;
        s_send_task_busy_us = 0;
        s_sample_lateness_max_us = 0;
        s_missed_deadlines = 0;

        uint32_t load_percent = elapsed_us > 0
                              ? (uint32_t)(((uint64_t)busy_us * 100u) / elapsed_us)
                              : 0;
        if (load_percent > 100u) {
            load_percent = 100u;
        }

        uint32_t queue_depth = sample_tx_queue ? uxQueueMessagesWaiting(sample_tx_queue) : 0;
        uint32_t free_heap_kb = esp_get_free_heap_size() / 1024u;

        send_node_health(u32_to_u8_saturating(load_percent),
                         u32_to_u8_saturating((max_lateness_us + 999u) / 1000u),
                         u32_to_u8_saturating(queue_depth),
                         u32_to_u8_saturating(missed_deadlines),
                         u32_to_u8_saturating(s_tx_fail_count),
                         u32_to_u8_saturating(free_heap_kb));

        report_count++;
        if (node_log_status_enabled() && (report_count % 5u) == 0u) {
            ESP_LOGI(TAG,
                     "HEALTH | active=%s load=%" PRIu32 "%% late_max=%" PRIu32
                     "us missed=%" PRIu32 " q=%" PRIu32 " drops=%" PRIu32
                     " tx_fail=%" PRIu32 " heap=%" PRIu32 "KB",
                     s_node_active ? "true" : "false",
                     load_percent,
                     max_lateness_us,
                     missed_deadlines,
                     queue_depth,
                     s_sample_drop_count,
                     s_tx_fail_count,
                     free_heap_kb);
        }
    }
}

static const char *twai_state_name(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE:
        return "active";
    case TWAI_ERROR_WARNING:
        return "warning";
    case TWAI_ERROR_PASSIVE:
        return "passive";
    case TWAI_ERROR_BUS_OFF:
        return "bus-off";
    default:
        return "unknown";
    }
}

static void can_recovery_task(void *arg)
{
    (void)arg;
    bool recovery_started = false;
    twai_error_state_t last_state = TWAI_ERROR_ACTIVE;

    while (1) {
        twai_node_status_t status = {};
        twai_node_record_t record = {};
        esp_err_t err = twai_node_get_info(node_hdl, &status, &record);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TWAI status read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_POLL_MS));
            continue;
        }

        if (status.state != last_state) {
            ESP_LOGW(TAG,
                     "TWAI state %s -> %s | tx_err=%u rx_err=%u bus_errors=%" PRIu32,
                     twai_state_name(last_state),
                     twai_state_name(status.state),
                     status.tx_error_count,
                     status.rx_error_count,
                     record.bus_err_num);
            last_state = status.state;
        }

        if (status.state == TWAI_ERROR_BUS_OFF && !recovery_started) {
            xQueueReset(sample_tx_queue);
            err = twai_node_recover(node_hdl);
            if (err == ESP_OK) {
                recovery_started = true;
                ESP_LOGW(TAG, "TWAI bus-off recovery started");
            } else {
                ESP_LOGE(TAG, "TWAI bus-off recovery failed to start: %s", esp_err_to_name(err));
            }
        } else if (recovery_started && status.state == TWAI_ERROR_ACTIVE) {
            recovery_started = false;
            ESP_LOGI(TAG, "TWAI bus recovered and is error-active");
            send_node_state(s_node_active ? NODE_STATE_ACTIVE : NODE_STATE_LOW_POWER,
                            NODE_STATE_REASON_RECOVERY);
        }

        vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_POLL_MS));
    }
}

static void wait_until_node_active(void)
{
    while (!s_node_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/* --------------------- Simulated sensor waveforms ------------------ */

static int32_t triangle_wave(uint32_t t_ms,
                             uint32_t period_ms,
                             int32_t min_value,
                             int32_t max_value)
{
    if (period_ms < 2 || max_value <= min_value) {
        return min_value;
    }

    uint32_t phase = t_ms % period_ms;
    uint32_t half_period = period_ms / 2;
    int32_t range = max_value - min_value;

    if (phase < half_period) {
        return min_value + (int32_t)(((int64_t)range * phase) / half_period);
    }

    return max_value -
           (int32_t)(((int64_t)range * (phase - half_period)) /
                     (period_ms - half_period));
}

static int32_t simulated_steering_deg_x10(uint32_t t_ms)
{
    // Offset steering angle: 450 means centered, roughly +/-35 degrees around it.
    int32_t slow_sweep = triangle_wave(t_ms, 7000, 100, 800);
    int32_t quick_correction = triangle_wave(t_ms + 900, 1200, -35, 35);
    return slow_sweep + quick_correction;
}

static int32_t simulated_brake_psi_x10(uint32_t t_ms)
{
    // Short brake applications with a fast ramp-up and slower release.
    uint32_t phase = t_ms % 5200;
    if (phase < 450) {
        return (int32_t)((1400u * phase) / 450u);
    }
    if (phase < 1800) {
        return 1400 - (int32_t)((1200u * (phase - 450u)) / 1350u);
    }

    return triangle_wave(t_ms + 250, 1800, 0, 80);
}

static int32_t simulated_wheel_rpm(uint32_t t_ms)
{
    int32_t base = triangle_wave(t_ms + 1400, 9000, 900, 4200);
    int32_t tire_noise = triangle_wave(t_ms, 650, -120, 120);
    return base + tire_noise;
}

static int32_t simulated_sensor_value(uint8_t sensor_id, uint32_t t_ms)
{
    switch (sensor_id) {
    case 20:
        return simulated_steering_deg_x10(t_ms);
    case 21:
        return simulated_brake_psi_x10(t_ms);
    case 22:
        return simulated_wheel_rpm(t_ms);
    default:
        return 0;
    }
}

static int32_t sample_adc_value_mv(const virtual_sensor_t *sensor)
{
    adc_channel_t channel;
    if (!gpio_to_adc_channel(sensor->port, &channel)) {
        return 0;
    }

    int voltage_mv = 0;
    esp_err_t err = adc_read_voltage_mv(channel, &voltage_mv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "ADC read failed sensor=%u GPIO%u: %s",
                 sensor->sensor_id,
                 sensor->port,
                 esp_err_to_name(err));
        return 0;
    }
    return voltage_mv;
}

static int32_t sample_brake_pressure_psi(const virtual_sensor_t *sensor,
                                         const brake_pressure_config_t *config)
{
    if (!config) {
        return 0;
    }

    int32_t voltage_mv = sample_adc_value_mv(sensor);
    float error_voltage_threshold =
        config->min_sensor_voltage_mv * BRAKE_SENSOR_DIVIDER_SCALE;
    float voltage_span =
        (BRAKE_SENSOR_DIVIDER_SCALE * config->max_sensor_voltage_mv) -
        error_voltage_threshold;

    if (voltage_span <= 0.0f) {
        ESP_LOGW(TAG, "%s invalid brake calibration span", config->caption);
        return 0;
    }

    float pressure =
        (((float)voltage_mv - error_voltage_threshold) / voltage_span) *
        config->pressure_span_psi;

    if (pressure < 0.0f) {
        pressure = 0.0f;
    }

    if (node_log_samples_enabled()) {
        ESP_LOGI(TAG,
                 "%s Pressure: %.2f psi, %" PRIi32 " mV",
                 config->caption,
                 pressure,
                 voltage_mv);
    }

    return (int32_t)pressure;
}

static int32_t sample_acceleration_g(const virtual_sensor_t *sensor,
                                     const acceleration_axis_config_t *config)
{
    if (!config) {
        return 0;
    }

    int32_t voltage_mv = sample_adc_value_mv(sensor);
    float g = ((float)voltage_mv - config->zero_g_voltage_mv) /
              ADXL377_SENSITIVITY_MV_PER_G;

    if (node_log_samples_enabled()) {
        ESP_LOGI(TAG,
                 "%s Acceleration: %.2f g, %" PRIi32 " mV",
                 config->caption,
                 g,
                 voltage_mv);
    }

    return (int32_t)g;
}

static int32_t sample_rpm_value(virtual_sensor_t *sensor)
{
    int64_t now_us = esp_timer_get_time() + g_offset_us;
    int32_t value = 0;

    portENTER_CRITICAL(&s_rpm_lock);
    if (sensor->rpm_last_pulse_us != 0 &&
        (now_us - sensor->rpm_last_pulse_us) < RPM_ZERO_TIMEOUT_US) {
        value = sensor->rpm_last_value;
    } else {
        sensor->rpm_last_value = 0;
        sensor->rpm_last_pulse_us = 0;
    }
    portEXIT_CRITICAL(&s_rpm_lock);

    return value;
}

static int32_t sample_old_rpm_value(virtual_sensor_t *sensor)
{
    static int64_t s_last_adc_rpm_log_us = 0;
    adc_channel_t channel;
    if (!gpio_to_adc_channel(sensor->port, &channel)) {
        return sensor->rpm_last_value;
    }

    int voltage_mv = 0;
    if (adc_read_voltage_mv(channel, &voltage_mv) != ESP_OK) {
        return sensor->rpm_last_value;
    }

    int64_t now_us = esp_timer_get_time() + g_offset_us;
    const int64_t minimum_pulse_interval_us =
        60000000LL / (RPM_MAX_VALID * PULSES_PER_ROTATION);

    if (NODE_SERIAL_TEST_MODE && node_log_samples_enabled() &&
        (now_us - s_last_adc_rpm_log_us) >= 250000) {
        ESP_LOGI(TAG,
                 "ADC RPM probe | GPIO%u voltage=%dmV threshold=%dmV rpm=%ld",
                 sensor->port,
                 voltage_mv,
                 OLD_RPM_DETECT_THRESHOLD_MV,
                 (long)sensor->rpm_last_value);
        s_last_adc_rpm_log_us = now_us;
    }

    if (voltage_mv < OLD_RPM_DETECT_THRESHOLD_MV && !sensor->rpm_peak_detected) {
        sensor->rpm_peak_detected = true;

        if (sensor->rpm_last_pulse_us != 0) {
            int64_t delta_us = now_us - sensor->rpm_last_pulse_us;
            if (delta_us >= minimum_pulse_interval_us) {
                sensor->rpm_last_value =
                    (int32_t)(60000000LL / (delta_us * PULSES_PER_ROTATION));
                sensor->rpm_last_pulse_us = now_us;
            }
        } else {
            sensor->rpm_last_pulse_us = now_us;
        }
    } else if (voltage_mv > OLD_RPM_RESET_THRESHOLD_MV && sensor->rpm_peak_detected) {
        sensor->rpm_peak_detected = false;
    }

    if (sensor->rpm_last_pulse_us != 0 &&
        (now_us - sensor->rpm_last_pulse_us) >= RPM_ZERO_TIMEOUT_US) {
        sensor->rpm_last_value = 0;
        sensor->rpm_last_pulse_us = 0;
    }

    return sensor->rpm_last_value;
}

static int32_t sample_bearing_encoder_value(virtual_sensor_t *sensor)
{
    int32_t count = 0;

    portENTER_CRITICAL(&s_encoder_lock);
    count = sensor->encoder_count;
    portEXIT_CRITICAL(&s_encoder_lock);

    int32_t degrees_x10 =
        (int32_t)(((int64_t)count * 360LL * BEARING_DEG_SCALE) /
                  BEARING_ENCODER_COUNTS_PER_ROTATION);

    if (node_log_samples_enabled()) {
        ESP_LOGI(TAG,
                 "Bearing encoder: count=%" PRIi32 " displacement=%" PRIi32 ".%01" PRIi32 " deg",
                 count,
                 degrees_x10 / BEARING_DEG_SCALE,
                 abs(degrees_x10 % BEARING_DEG_SCALE));
    }

    return degrees_x10;
}

static int32_t sample_sensor_value(virtual_sensor_t *sensor, uint32_t t_ms)
{
    switch (sensor->function) {
    case NODE_SENSOR_FUNCTION_ADC:
        return sample_adc_value_mv(sensor);
    case NODE_SENSOR_FUNCTION_RPM:
        return sample_rpm_value(sensor);
    case NODE_SENSOR_FUNCTION_OLD_RPM:
        return sample_old_rpm_value(sensor);
    case NODE_SENSOR_FUNCTION_FRONT_BRAKE:
        return sample_brake_pressure_psi(sensor, &FRONT_BRAKE_CONFIG);
    case NODE_SENSOR_FUNCTION_REAR_BRAKE:
        return sample_brake_pressure_psi(sensor, &REAR_BRAKE_CONFIG);
    case NODE_SENSOR_FUNCTION_ACCEL_X:
        return sample_acceleration_g(sensor, &ACCEL_X_CONFIG);
    case NODE_SENSOR_FUNCTION_ACCEL_Y:
        return sample_acceleration_g(sensor, &ACCEL_Y_CONFIG);
    case NODE_SENSOR_FUNCTION_ACCEL_Z:
        return sample_acceleration_g(sensor, &ACCEL_Z_CONFIG);
    case NODE_SENSOR_FUNCTION_BEARING:
        return sample_bearing_encoder_value(sensor);
    case NODE_SENSOR_FUNCTION_SIM:
    default:
        return simulated_sensor_value(sensor->sensor_id, t_ms);
    }
}

/* --------------------- Sampling tasks ------------------ */

static void sample_task(void *arg)
{
    /*
    Simulated multi-sensor sampler. This represents one physical node publishing
    three independent virtual sensor streams, each on its own CAN ID.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    (void)arg;
    int64_t last_log_us = 0;
#if NODE_SERIAL_TEST_MODE
    int64_t last_idle_yield_us = 0;
#endif

    while (1)
    {
        wait_until_node_active();
        arm_sensor_schedule();

        while (s_node_active) {
            int64_t loop_start_us = esp_timer_get_time();
            int32_t timestamp = get_synced_timestamp_ms();
            uint32_t t_ms = (uint32_t)timestamp;
            int64_t now_us = loop_start_us;
            int64_t next_due_us = now_us + 1000000;

            for (size_t i = 0; i < MAX_VIRTUAL_SENSORS; i++) {
                virtual_sensor_t *sensor = &s_virtual_sensors[i];
                if (sensor->sensor_id == 0 || !sensor->enabled) {
                    continue;
                }

                if (sensor->next_sample_us == 0) {
                    sensor->next_sample_us = now_us;
                }
                if (now_us < sensor->next_sample_us) {
                    if (sensor->next_sample_us < next_due_us) {
                        next_due_us = sensor->next_sample_us;
                    }
                    continue;
                }

                int64_t lateness_us = now_us - sensor->next_sample_us;
                if (lateness_us > 0) {
                    record_sample_lateness_us((uint32_t)lateness_us);
                }

                do {
                    sensor->next_sample_us += sensor->period_us;
                } while (sensor->next_sample_us <= now_us);
                if (sensor->next_sample_us < next_due_us) {
                    next_due_us = sensor->next_sample_us;
                }

                int32_t value = sample_sensor_value(sensor, t_ms);
                queue_sample(sensor->can_id, timestamp, value);
            }

            now_us = esp_timer_get_time();
            if (node_log_status_enabled() && (now_us - last_log_us) >= 1000000) {
                ESP_LOGI(TAG,
                         "SAMPLE | configured sensors=%u",
                         (unsigned)MAX_VIRTUAL_SENSORS);
                last_log_us = now_us;
            }

            record_sample_loop_busy_time((uint32_t)(esp_timer_get_time() - loop_start_us));

#if NODE_SERIAL_TEST_MODE
            now_us = esp_timer_get_time();
            if ((now_us - last_idle_yield_us) >= 50000) {
                last_idle_yield_us = now_us;
                vTaskDelay(1);
                continue;
            }
#endif

            int64_t sleep_us = next_due_us - esp_timer_get_time();
            if (sleep_us >= 2000) {
                vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
            } else if (sleep_us > SIM_TASK_IDLE_US) {
                esp_rom_delay_us((uint32_t)(sleep_us - SIM_TASK_IDLE_US));
            } else {
                taskYIELD();
            }
        }
    }
}

static void sample_adc_task(void *arg)
{
    /*
    This task samples the ADC and pushes timestamped pressure values into
    sample_tx_queue.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    while (1)
    {
        wait_until_node_active();
        TickType_t last_wake = xTaskGetTickCount();

        while (s_node_active) {
            int raw = 0;
            if (adc_read_raw(ADC_CH_USED, &raw) != ESP_OK) {
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS));
                continue;
            }

            int32_t timestamp = get_synced_timestamp_ms();
            int value_mv = 0;
            if (adc_read_voltage_mv(ADC_CH_USED, &value_mv) != ESP_OK) {
                value_mv = 0;
            }

            queue_sample(ID_SENSOR_BRAKE, timestamp, value_mv);
            ESP_LOGI(TAG,
                        "adc=%dmV raw=%d",
                        value_mv, raw);

            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS));
        }
    }
}

#if ENABLE_RPM_SAMPLE_TASK
static void sample_rpm_task(void *arg)
{
    /*
    This task samples the ADC pulse, detects each pulse crossing, and computes RPM
    from the time between the current pulse and the previous one.
    */

    while (1)
    {
        wait_until_node_active();

        bool peak_detected = false;
        int64_t last_pulse_us = 0;
        int64_t last_log_us = 0;
        int voltage_mv = 0;
        const int64_t minimum_pulse_interval_us =
            60000000LL / (RPM_MAX_VALID * PULSES_PER_ROTATION);
        TickType_t last_wake = xTaskGetTickCount();

        while (s_node_active) {
            if (adc_read_voltage_mv(ADC_CH_USED, &voltage_mv) != ESP_OK) {
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));
                continue;
            }
            

            // Detect one pulse when the signal crosses below the voltage threshold.
            if (voltage_mv < OLD_RPM_DETECT_THRESHOLD_MV && !peak_detected) {
                peak_detected = true;

                int64_t t_main_sample_us = esp_timer_get_time() + g_offset_us;
                int32_t timestamp = (int32_t)(t_main_sample_us / 1000); // Convert from us to ms
                int64_t delta_us = 0;
                int32_t rpm = 0;

                if (last_pulse_us != 0) {
                    delta_us = t_main_sample_us - last_pulse_us;
                    if (delta_us >= minimum_pulse_interval_us) {
                        rpm = (int32_t)(60000000LL / (delta_us * PULSES_PER_ROTATION));
                        last_pulse_us = t_main_sample_us;
                    } else {
                        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));
                        continue;
                    }
                } else {
                    last_pulse_us = t_main_sample_us;
                }

                queue_sample(ID_SENSOR_WHEEL_RPM, timestamp, rpm);

                if (delta_us > 0 && (t_main_sample_us - last_log_us) >= 100000) {
                    ESP_LOGI(TAG, "Pulse detected | voltage=%dmV dt=%lld us rpm=%ld",
                            voltage_mv, delta_us, (long)rpm);
                    last_log_us = t_main_sample_us;
                } else if (delta_us == 0) {
                    ESP_LOGI(TAG, "First pulse detected | voltage=%dmV waiting for next pulse", voltage_mv);
                }

                
            }
            // Use a higher reset threshold to avoid retriggering on noisy edges.
            else if (voltage_mv > OLD_RPM_RESET_THRESHOLD_MV && peak_detected) {
                peak_detected = false;
            }

            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));

        }
    }
}
#endif

/* --------------------- CAN send/receive tasks ------------------ */

static void send_task(void *arg)
{
    /*
    Sends queued samples over CAN. Sampling and sending are decoupled so samplers
    can continue collecting while the bus is busy.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    TickType_t last_send = xTaskGetTickCount();
#if NODE_SERIAL_TEST_MODE || NODE_SERIAL_DEBUG_SAMPLES
    int64_t last_serial_sample_log_us = 0;
#endif

    while (1)
    {
        sample_data_t sample;
        xQueueReceive(sample_tx_queue, &sample, portMAX_DELAY);

        if (!s_node_active) {
            continue;
        }

        int64_t send_start_us = esp_timer_get_time();
#if NODE_SERIAL_TEST_MODE || NODE_SERIAL_DEBUG_SAMPLES
        if ((send_start_us - last_serial_sample_log_us) >=
            (SERIAL_SAMPLE_LOG_PERIOD_MS * 1000LL)) {
            ESP_LOGI(TAG,
                     "SERIAL SAMPLE | id=0x%03" PRIX32
                     " sensor_ts_ms=%" PRIi32 " value=%" PRIi32,
                     sample.can_id,
                     sample.timestamp_ms,
                     sample.value);
            last_serial_sample_log_us = send_start_us;
        }
#endif
#if !NODE_SERIAL_TEST_MODE
        uint8_t raw[8];
        uint64_t data = pack_sample_data(&sample);
        memcpy(raw, &data, 8);

        twai_frame_t tx = {
            .header.id = sample.can_id,
            .header.ide = false,
            .buffer = raw,
            .buffer_len = 8,
        };

        esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(CAN_TX_TIMEOUT_MS));
        if (err == ESP_OK) {
            if (node_log_samples_enabled()) {
                ESP_LOGI(TAG,
                         "TX Data | id=0x%03" PRIX32
                         " ts=%" PRIi32 " val=%" PRIi32,
                         sample.can_id,
                         sample.timestamp_ms,
                         sample.value);
            }
        } else {
            s_tx_fail_count++;
            ESP_LOGW(TAG,
                     "TX Data failed | id=0x%03" PRIX32
                     " ts=%" PRIi32 " val=%" PRIi32 " err=%s",
                     sample.can_id,
                     sample.timestamp_ms,
                     sample.value,
                     esp_err_to_name(err));
        }
#endif
        record_send_task_busy_time((uint32_t)(esp_timer_get_time() - send_start_us));

        if (SEND_PERIOD_MS > 0) {
            vTaskDelayUntil(&last_send, pdMS_TO_TICKS(SEND_PERIOD_MS));
        }


    }

}

static void receive_task(void *arg)
{
    /*
    This task will sample the needed data, buffer it (and add a timestamp if it has
    P4 timestamp) and wait for the data to be sent before sampling again.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    int64_t last_beacon_log_us = 0;

    while (1)
    {
        can_rx_word_t rx_msg;
        xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);
        if (rx_msg.id == ID_MASTER_TIME_BEACON) {
            
            handle_time_beacon(&rx_msg);
            int64_t now_us = esp_timer_get_time();
            if (node_log_status_enabled() && (now_us - last_beacon_log_us) >= 1000000) {
                ESP_LOGI(TAG,
                    "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32,
                    rx_msg.id,
                    (uint32_t)rx_msg.data);
                last_beacon_log_us = now_us;
            }

        }
        else if (rx_msg.id == ID_NODE_CONFIG_CMD) {
            handle_node_config_frame(&rx_msg);
        }
        else if (rx_msg.id == ID_START_CMD) {
            if (rx_targets_this_node(&rx_msg)) {
                xQueueReset(sample_tx_queue);
                arm_sensor_schedule();
                s_node_active = true;
                ESP_LOGI(TAG,
                         "START received target=%u dlc=%u - sampling enabled",
                         rx_msg.dlc > 0 ? rx_byte(&rx_msg, 0) : 0xFF,
                         rx_msg.dlc);
                send_node_state(NODE_STATE_ACTIVE, NODE_STATE_REASON_START);
            }
        }
        else if (rx_msg.id == ID_STOP_CMD) {
            if (rx_targets_this_node(&rx_msg)) {
                if (s_node_active && rx_is_broadcast_command(&rx_msg)) {
                    ESP_LOGW(TAG,
                             "Ignoring broadcast STOP while active | dlc=%u data=0x%016" PRIX64,
                             rx_msg.dlc,
                             rx_msg.data);
                    continue;
                }

                if (s_node_active) {
                    s_node_active = false;
                    xQueueReset(sample_tx_queue);
                    ESP_LOGI(TAG,
                             "STOP received target=%u dlc=%u - sampling disabled",
                             rx_msg.dlc > 0 ? rx_byte(&rx_msg, 0) : 0xFF,
                             rx_msg.dlc);
                    send_node_state(NODE_STATE_LOW_POWER, NODE_STATE_REASON_STOP);
                } else {
                    ESP_LOGI(TAG,
                             "STOP received while already idle target=%u dlc=%u",
                             rx_msg.dlc > 0 ? rx_byte(&rx_msg, 0) : 0xFF,
                             rx_msg.dlc);
                    send_node_state(NODE_STATE_LOW_POWER, NODE_STATE_REASON_STOP);
                }
            }
        }
        
        
    }
    


}

/* --------------------- Application entry ------------------ */

void app_main(void)
{
    s_boot_reset_reason = (uint8_t)esp_reset_reason();
    ESP_LOGI(TAG,
             "Boot reset reason: %s (%u)",
             reset_reason_name((esp_reset_reason_t)s_boot_reset_reason),
             s_boot_reset_reason);

    ESP_ERROR_CHECK(adc_oneshot_init());
    configure_enabled_adc_sensors();
    configure_enabled_rpm_sensors();
    configure_enabled_bearing_sensors();

    //Create tasks, queues, and semaphores
    sample_tx_queue = xQueueCreate(SAMPLE_QUEUE_LENGTH, sizeof(sample_data_t));
    ESP_ERROR_CHECK(sample_tx_queue == NULL ? ESP_FAIL : ESP_OK);

#if NODE_SERIAL_TEST_MODE
    s_node_active = true;
    s_log_mode = NODE_LOG_MODE_SAMPLES;
    arm_sensor_schedule();
    ESP_LOGW(TAG, "NODE_SERIAL_TEST_MODE enabled - CAN/TWAI disabled, printing samples to serial");
#else
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    ESP_ERROR_CHECK(twai_rx_queue == NULL ? ESP_FAIL : ESP_OK);

    //Install TWAI driver
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(TAG, "Driver installed");
    
    // Handle CAN receive ISR
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

    /* ---------- Enable / start TWAI ---------- */
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
#if NODE_START_ACTIVE_ON_BOOT
    s_node_active = true;
    arm_sensor_schedule();
    ESP_LOGW(TAG, "TWAI node enabled; NODE_START_ACTIVE_ON_BOOT set - sampling immediately");
#else
    ESP_LOGI(TAG, "TWAI node enabled; waiting for START command");
    send_node_state(NODE_STATE_LOW_POWER, NODE_STATE_REASON_BOOT);
#endif
#endif

    xTaskCreatePinnedToCore(send_task, "send", 8192, NULL, 7, NULL, tskNO_AFFINITY);
#if !NODE_SERIAL_TEST_MODE
    xTaskCreatePinnedToCore(can_recovery_task, "can_recovery", 4096, NULL, 8, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(health_task, "health", 4096, NULL, 3, NULL, tskNO_AFFINITY);
#endif
#if ENABLE_SIMULATED_SENSOR_TASK
    xTaskCreatePinnedToCore(sample_task, "sample", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
#if ENABLE_ADC_SAMPLE_TASK
    xTaskCreatePinnedToCore(sample_adc_task, "sample_adc", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
#if ENABLE_RPM_SAMPLE_TASK
    xTaskCreatePinnedToCore(sample_rpm_task, "sample_rpm", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
#if !NODE_SERIAL_TEST_MODE
    xTaskCreatePinnedToCore(receive_task, "receive", 4096, NULL, 8, NULL, tskNO_AFFINITY);
#endif
}
