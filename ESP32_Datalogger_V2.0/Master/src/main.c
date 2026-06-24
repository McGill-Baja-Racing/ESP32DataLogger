
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "mbedtls/base64.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"

#include "master_control.h"
#include "gps.h"
#include "rpm_sampler.h"
#include "telemetry.h"
#include "wifi_control.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "Master";

#ifndef MASTER_CAN_ENABLED
#define MASTER_CAN_ENABLED 1
#endif

#ifndef MASTER_AUTO_START_LOGGING
#define MASTER_AUTO_START_LOGGING 1
#endif

#ifndef MASTER_WIFI_CONTROL_ENABLED
#define MASTER_WIFI_CONTROL_ENABLED 0
#endif

#ifndef MASTER_USE_SD_NODE_CONFIG
#define MASTER_USE_SD_NODE_CONFIG 0
#endif

#if MASTER_CAN_ENABLED
#define MASTER_CAN_ENABLED_JSON "true"
#else
#define MASTER_CAN_ENABLED_JSON "false"
#endif

/*
 * Master firmware map
 * -------------------
 * This file owns the local logger: SD card, TWAI/CAN receive, node runtime
 * configuration, serial commands, and JSON helpers used by telemetry.c.
 *
 * Most common edit points:
 * - CAN IDs and pins: "System constants" and "TWAI/CAN config" below.
 * - Built-in fallback nodes/sensors: load_default_node_config().
 * - SD binary log format: sd_writer_task() writes [CAN ID, packed data].
 * - Start/stop behavior: master_start_logging(), master_stop_logging(),
 *   and send_runtime_config_and_start_nodes().
 * - JSON returned to Wi-Fi/MQTT dashboard: master_format_*_json().
 *
 * Runtime data path:
 * CAN ISR -> twai_rx_queue -> rx_dispatch_task()
 *    -> telemetry_submit_can_frame() for live MQTT preview
 *    -> twai_rx_sample_queue -> sd_writer_task() for SD binary logs
 */

/* --------------------- System constants ------------------ */
#define INFO_PER_SAMPLE  2          // CAN ID, Data (Timestamp + value)
#define SAMPLES_PER_BLOCK  100
#define LOG_SAVE_INTERVAL_MS 15000

// Shared CAN protocol IDs. Node code must use the same command/state IDs.
#define ID_MASTER_TIME_BEACON   0x0A2
#define ID_NODE1_HEARTBEAT      0x000 // TO DO SET THIS TO A GOOD VALUE
#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_NODE_CONFIG_CMD      0x0A3
#define ID_NODE_STATE_BASE      0x0C0
#define ID_NODE_HEALTH_BASE     0x180
#define NODE_STATE_LOW_POWER    0
#define NODE_STATE_ACTIVE       1
#define NODE_STATE_REASON_BOOT  1
#define NODE_STATE_REASON_STOP  2
#define NODE_STATE_REASON_START 3
#define NODE_STATE_REASON_RECOVERY 4
#define NODE_LOW_POWER_ACK_WAIT_MS 1000
#define NODE_CONFIG_CMD_RESET   0
#define NODE_CONFIG_CMD_SENSOR  1
#define NODE_CONFIG_CMD_LOG     2
#define NODE_CONFIG_CMD_SENSOR_IO 3
#define DEFAULT_SAMPLE_RATE_HZ  10

#define TIME_BEACON_PERIOD_MS   100
#define MASTER_AUTO_START_DELAY_MS 500
#define CAN_RECOVERY_POLL_MS    250
#define NODE_OFFLINE_TIMEOUT_MS 3000
#define NODE_RETRY_INTERVAL_MS  3000
#define LOG_PATH_MAX_LEN        64
#define DOWNLOAD_READ_CHUNK_BYTES 24
#define DOWNLOAD_B64_CHUNK_BYTES  80
#define DOWNLOAD_SERIAL_LINE_DELAY_MS 20
#define NODES_CONFIG_PATH      "/sdcard/nodes_config.json"
#define NODES_CONFIG_TMP_PATH  "/sdcard/nodes_config.tmp"
#define NODES_CONFIG_BAK_PATH  "/sdcard/nodes_config.bak"
#define NODES_CONFIG_MAX_BYTES 16384
#define MQTT_CONFIG_PATH       "/sdcard/mqtt_config.json"
#define MQTT_CONFIG_TMP_PATH   "/sdcard/mqtt_config.tmp"
#define MQTT_CONFIG_MAX_BYTES  256
#define MAX_CONFIGURED_NODES   16
#define MAX_NODE_SENSORS        8
#define MAX_MASTER_SENSORS      8
#define NODE_NAME_MAX_LEN      32
#define SENSOR_NAME_MAX_LEN    32
#define SENSOR_UNITS_MAX_LEN   16
#define SENSOR_FUNCTION_MAX_LEN 16
#define MASTER_RPM_DEFAULT_GPIO 21

/* --------------------- Logger state ------------------ */

static SemaphoreHandle_t log_file_mutex = NULL;

static FILE *logFile = NULL;
static int writeCounter = 0;

typedef enum {
    LOG_STATE_IDLE = 0,
    LOG_STATE_RUNNING,
    LOG_STATE_STOPPING,
} log_state_t;

static volatile log_state_t log_state = LOG_STATE_IDLE;
static char current_log_path[LOG_PATH_MAX_LEN];

static TaskHandle_t dispatch_task_handle = NULL;
static TaskHandle_t sd_task_handle     = NULL;
static TaskHandle_t save_task_handle   = NULL;
static TaskHandle_t serial_task_handle = NULL;
static TaskHandle_t can_recovery_task_handle = NULL;
static TaskHandle_t node_watchdog_task_handle = NULL;

/* --------------------- TWAI/CAN config ------------------ */
#define TX_GPIO_NUM             20
#define RX_GPIO_NUM             21
#define TRANSM_RATE             1000000
#define TX_QUEUE_DEPTH          5
#define RX_QUEUE_LENGTH         256

#define LOG_MODE_OFF            0x00
#define LOG_MODE_MASTER_SAMPLES 0x01
#define LOG_MODE_NODE_STATUS    0x02
#define LOG_MODE_NODE_SAMPLES   0x04
#define NODE_LOG_MODE_STATUS    0x01
#define NODE_LOG_MODE_SAMPLES   0x02

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

typedef struct {
    uint8_t sensor_id;
    char name[SENSOR_NAME_MAX_LEN];
    uint32_t can_id;
    char units[SENSOR_UNITS_MAX_LEN];
    uint16_t sample_rate_hz;
    char function[SENSOR_FUNCTION_MAX_LEN];
    uint8_t port;
    uint8_t aux_port;
    bool preview_enabled;
} sensor_config_t;

typedef struct {
    uint8_t node_id;
    char name[NODE_NAME_MAX_LEN];
    uint32_t state_can_id;
    bool active;
    bool low_power_ack_seen;
    int64_t last_seen_rx_us;
    int64_t last_state_rx_us;
    int64_t last_recovery_attempt_us;
    sensor_config_t sensors[MAX_NODE_SENSORS];
    size_t sensor_count;
} node_config_entry_t;

/* --------------------- Global runtime state ------------------ */

static node_config_entry_t configured_nodes[MAX_CONFIGURED_NODES];
static size_t configured_node_count = 0;
static sensor_config_t configured_master_sensors[MAX_MASTER_SENSORS];
static size_t configured_master_sensor_count = 0;
static bool node_config_loaded_from_sd = false;

static QueueHandle_t twai_rx_queue = NULL;
static QueueHandle_t twai_rx_heartbeat_queue = NULL;
static QueueHandle_t twai_rx_sample_queue = NULL;
static twai_node_handle_t node_hdl = NULL;

static volatile uint32_t rx_heartbeat_queue_drops = 0;
static volatile uint32_t rx_sample_queue_drops = 0;
static volatile uint32_t sd_write_failures = 0;
static volatile uint32_t sd_write_last_ms = 0;
static volatile uint32_t sd_write_max_ms = 0;
static volatile uint32_t can_rx_frames_interval = 0;
static volatile uint32_t can_rx_bits_interval = 0;
static volatile uint32_t can_tx_frames_interval = 0;
static volatile uint32_t can_tx_bits_interval = 0;
static volatile uint32_t dispatch_busy_us_interval = 0;
static volatile uint32_t sd_writer_busy_us_interval = 0;
static portMUX_TYPE health_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t health_last_report_us = 0;
static volatile uint8_t runtime_log_mode = LOG_MODE_OFF;
static volatile bool can_recovered_since_bus_off = false;

static twai_onchip_node_config_t node_config = {
    .io_cfg = {
        .tx = (gpio_num_t)TX_GPIO_NUM,
        .rx = (gpio_num_t)RX_GPIO_NUM,
    },
    .bit_timing = {
        .bitrate = TRANSM_RATE,
    },
    .tx_queue_depth = TX_QUEUE_DEPTH,
};

static bool master_can_ready(void)
{
    return MASTER_CAN_ENABLED && node_hdl != NULL;
}

static void print_node_power_summary(void);
static const char *log_state_name(log_state_t state);
static esp_err_t create_node_config_file_from_current(void);

/* --------------------- Health/load accounting helpers ------------------ */

static uint32_t estimate_can_frame_bits(uint8_t dlc)
{
    if (dlc > 8) {
        dlc = 8;
    }

    /*
     * Standard 11-bit CAN frame estimate, including inter-frame spacing plus
     * about 20% for bit stuffing. This is an estimate for load tracking, not a
     * protocol analyzer measurement.
     */
    uint32_t base_bits = 47u + ((uint32_t)dlc * 8u);
    return base_bits + (base_bits / 5u);
}

static void record_can_rx_frame(uint8_t dlc)
{
    uint32_t bits = estimate_can_frame_bits(dlc);
    portENTER_CRITICAL_ISR(&health_stats_lock);
    can_rx_frames_interval++;
    can_rx_bits_interval += bits;
    portEXIT_CRITICAL_ISR(&health_stats_lock);
}

static void record_can_tx_frame(uint8_t dlc)
{
    uint32_t bits = estimate_can_frame_bits(dlc);
    portENTER_CRITICAL(&health_stats_lock);
    can_tx_frames_interval++;
    can_tx_bits_interval += bits;
    portEXIT_CRITICAL(&health_stats_lock);
}

static void record_interval_busy_us(volatile uint32_t *counter, uint32_t busy_us)
{
    portENTER_CRITICAL(&health_stats_lock);
    uint32_t total = *counter + busy_us;
    *counter = total < *counter ? UINT32_MAX : total;
    portEXIT_CRITICAL(&health_stats_lock);
}

static bool master_log_samples_enabled(void)
{
    return (runtime_log_mode & LOG_MODE_MASTER_SAMPLES) != 0;
}

static const char *master_log_mode_name(uint8_t mode)
{
    switch (mode) {
    case LOG_MODE_OFF:
        return "off";
    case LOG_MODE_MASTER_SAMPLES:
        return "master";
    case LOG_MODE_NODE_STATUS:
        return "status";
    case LOG_MODE_NODE_SAMPLES:
        return "node";
    case LOG_MODE_MASTER_SAMPLES | LOG_MODE_NODE_SAMPLES:
        return "samples";
    case LOG_MODE_MASTER_SAMPLES | LOG_MODE_NODE_STATUS | LOG_MODE_NODE_SAMPLES:
        return "all";
    default:
        return "custom";
    }
}

static uint8_t parse_log_mode_text(const char *mode_text, bool *ok)
{
    char mode[16] = {};
    if (mode_text) {
        snprintf(mode, sizeof(mode), "%s", mode_text);
    }

    char *start = mode;
    while (isspace((unsigned char)*start)) {
        start++;
    }

    for (char *p = start; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    if (strcmp(start, "off") == 0 || strcmp(start, "quiet") == 0 || strcmp(start, "0") == 0) {
        *ok = true;
        return LOG_MODE_OFF;
    }
    if (strcmp(start, "master") == 0 || strcmp(start, "rx") == 0) {
        *ok = true;
        return LOG_MODE_MASTER_SAMPLES;
    }
    if (strcmp(start, "status") == 0) {
        *ok = true;
        return LOG_MODE_NODE_STATUS;
    }
    if (strcmp(start, "node") == 0 || strcmp(start, "tx") == 0) {
        *ok = true;
        return LOG_MODE_NODE_SAMPLES;
    }
    if (strcmp(start, "sample") == 0 || strcmp(start, "samples") == 0) {
        *ok = true;
        return LOG_MODE_MASTER_SAMPLES | LOG_MODE_NODE_SAMPLES;
    }
    if (strcmp(start, "all") == 0 || strcmp(start, "on") == 0 || strcmp(start, "verbose") == 0) {
        *ok = true;
        return LOG_MODE_MASTER_SAMPLES | LOG_MODE_NODE_STATUS | LOG_MODE_NODE_SAMPLES;
    }

    *ok = false;
    return LOG_MODE_OFF;
}

/* --------------------- Node configuration JSON ------------------ */

/*
 * The master stores node/sensor configuration in /sdcard/nodes_config.json.
 * If the file is missing or invalid, load_default_node_config() keeps one
 * simulated node available so the rest of the system can still boot and test.
 */
static void copy_truncated(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    snprintf(dst, dst_size, "%s", src);
}

static uint32_t parse_can_id_or_default(const cJSON *item, uint32_t default_value)
{
    if (cJSON_IsNumber(item)) {
        return (uint32_t)item->valuedouble;
    }

    if (cJSON_IsString(item) && item->valuestring) {
        char *end = NULL;
        unsigned long parsed = strtoul(item->valuestring, &end, 0);
        if (end != item->valuestring && *end == '\0') {
            return (uint32_t)parsed;
        }
    }

    return default_value;
}

static bool json_bool_or_default(const cJSON *item, bool default_value)
{
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }

    return default_value;
}

static uint16_t json_u16_or_default(const cJSON *item, uint16_t default_value)
{
    if (!cJSON_IsNumber(item)) {
        return default_value;
    }

    int value = item->valueint;

    if (value < 1) {
        return default_value;
    }

    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }

    return (uint16_t)value;
}

static uint8_t parse_port_or_default(const cJSON *item, uint8_t default_value)
{
    int value = -1;

    if (cJSON_IsNumber(item)) {
        value = item->valueint;
    } else if (cJSON_IsString(item) && item->valuestring) {
        const char *text = item->valuestring;
        while (isspace((unsigned char)*text)) {
            text++;
        }
        if (strncasecmp(text, "GPIO", 4) == 0) {
            text += 4;
        }
        value = atoi(text);
    }

    if (value < 0 || value > UINT8_MAX) {
        return default_value;
    }

    return (uint8_t)value;
}

static uint8_t sensor_function_code(const char *function)
{
    if (!function) {
        return 0;
    }
    if (strcasecmp(function, "adc") == 0) {
        return 1;
    }
    if (strcasecmp(function, "rpm") == 0) {
        return 2;
    }
    if (strcasecmp(function, "front_brake") == 0 || strcasecmp(function, "front_b") == 0) {
        return 3;
    }
    if (strcasecmp(function, "rear_brake") == 0 || strcasecmp(function, "rear_br") == 0) {
        return 4;
    }
    if (strcasecmp(function, "old_rpm") == 0 || strcasecmp(function, "adc_rpm") == 0) {
        return 5;
    }
    if (strcasecmp(function, "bearing") == 0 || strcasecmp(function, "encoder") == 0) {
        return 6;
    }
    return 0;
}

static void configure_default_brake_node(size_t index, uint8_t node_id)
{
    node_config_entry_t *node = &configured_nodes[index];
    node->node_id = node_id;
    snprintf(node->name, sizeof(node->name), "brake_node_%u", node_id);
    node->state_can_id = ID_NODE_STATE_BASE + node_id;
    node->active = false;
    node->low_power_ack_seen = false;
    node->last_seen_rx_us = 0;
    node->last_state_rx_us = 0;
    node->last_recovery_attempt_us = 0;
    node->sensor_count = 2;

    node->sensors[0].sensor_id = 21;
    copy_truncated(node->sensors[0].name,
                   sizeof(node->sensors[0].name),
                   "front_brake_pressure");
    node->sensors[0].can_id = 0x0B1;
    copy_truncated(node->sensors[0].units,
                   sizeof(node->sensors[0].units),
                   "psi_x10");
    node->sensors[0].sample_rate_hz = 100;
    copy_truncated(node->sensors[0].function,
                   sizeof(node->sensors[0].function),
                   "front_brake");
    node->sensors[0].port = 1;
    node->sensors[0].aux_port = 0;
    node->sensors[0].preview_enabled = true;

    node->sensors[1].sensor_id = 23;
    copy_truncated(node->sensors[1].name,
                   sizeof(node->sensors[1].name),
                   "rear_brake_pressure");
    node->sensors[1].can_id = 0x0B2;
    copy_truncated(node->sensors[1].units,
                   sizeof(node->sensors[1].units),
                   "psi_x10");
    node->sensors[1].sample_rate_hz = 100;
    copy_truncated(node->sensors[1].function,
                   sizeof(node->sensors[1].function),
                   "rear_brake");
    node->sensors[1].port = 2;
    node->sensors[1].aux_port = 0;
    node->sensors[1].preview_enabled = true;
}

static void configure_default_encoder_node(size_t index, uint8_t node_id)
{
    node_config_entry_t *node = &configured_nodes[index];
    node->node_id = node_id;
    snprintf(node->name, sizeof(node->name), "encoder_node_%u", node_id);
    node->state_can_id = ID_NODE_STATE_BASE + node_id;
    node->active = false;
    node->low_power_ack_seen = false;
    node->last_seen_rx_us = 0;
    node->last_state_rx_us = 0;
    node->last_recovery_attempt_us = 0;
    node->sensor_count = 1;

    node->sensors[0].sensor_id = 33;
    copy_truncated(node->sensors[0].name,
                   sizeof(node->sensors[0].name),
                   "bearing_encoder");
    node->sensors[0].can_id = 0x0B9;
    copy_truncated(node->sensors[0].units,
                   sizeof(node->sensors[0].units),
                   "deg_x10");
    node->sensors[0].sample_rate_hz = 50;
    copy_truncated(node->sensors[0].function,
                   sizeof(node->sensors[0].function),
                   "bearing");
    node->sensors[0].port = 6;
    node->sensors[0].aux_port = 7;
    node->sensors[0].preview_enabled = true;
}

static void configure_default_master_sensors(void)
{
    memset(configured_master_sensors, 0, sizeof(configured_master_sensors));
    configured_master_sensor_count = 4;

    configured_master_sensors[0].sensor_id = 1;
    copy_truncated(configured_master_sensors[0].name,
                   sizeof(configured_master_sensors[0].name),
                   "gps_speed");
    configured_master_sensors[0].can_id = 0x700;
    copy_truncated(configured_master_sensors[0].units,
                   sizeof(configured_master_sensors[0].units),
                   "kph_x100");
    configured_master_sensors[0].sample_rate_hz = 1;
    configured_master_sensors[0].preview_enabled = true;
    copy_truncated(configured_master_sensors[0].function,
                   sizeof(configured_master_sensors[0].function),
                   "gps_speed");

    configured_master_sensors[1].sensor_id = 2;
    copy_truncated(configured_master_sensors[1].name,
                   sizeof(configured_master_sensors[1].name),
                   "gps_latitude");
    configured_master_sensors[1].can_id = 0x701;
    copy_truncated(configured_master_sensors[1].units,
                   sizeof(configured_master_sensors[1].units),
                   "deg_e7");
    configured_master_sensors[1].sample_rate_hz = 1;
    configured_master_sensors[1].preview_enabled = false;
    copy_truncated(configured_master_sensors[1].function,
                   sizeof(configured_master_sensors[1].function),
                   "gps_latitude");

    configured_master_sensors[2].sensor_id = 3;
    copy_truncated(configured_master_sensors[2].name,
                   sizeof(configured_master_sensors[2].name),
                   "gps_longitude");
    configured_master_sensors[2].can_id = 0x702;
    copy_truncated(configured_master_sensors[2].units,
                   sizeof(configured_master_sensors[2].units),
                   "deg_e7");
    configured_master_sensors[2].sample_rate_hz = 1;
    configured_master_sensors[2].preview_enabled = false;
    copy_truncated(configured_master_sensors[2].function,
                   sizeof(configured_master_sensors[2].function),
                   "gps_longitude");

    configured_master_sensors[3].sensor_id = 4;
    copy_truncated(configured_master_sensors[3].name,
                   sizeof(configured_master_sensors[3].name),
                   "engine_rpm");
    configured_master_sensors[3].can_id = 0x703;
    copy_truncated(configured_master_sensors[3].units,
                   sizeof(configured_master_sensors[3].units),
                   "rpm");
    configured_master_sensors[3].sample_rate_hz = 50;
    configured_master_sensors[3].port = MASTER_RPM_DEFAULT_GPIO;
    configured_master_sensors[3].aux_port = 0;
    configured_master_sensors[3].preview_enabled = true;
    copy_truncated(configured_master_sensors[3].function,
                   sizeof(configured_master_sensors[3].function),
                   "engine_rpm");
}

static bool master_sensor_preview_enabled(const sensor_config_t *sensor)
{
    return sensor && sensor->preview_enabled;
}

static void apply_gps_signal_config(void)
{
    gps_signal_config_t gps_signals[MAX_MASTER_SENSORS] = {};
    size_t gps_signal_count = 0;

    for (size_t i = 0; i < configured_master_sensor_count; i++) {
        const sensor_config_t *sensor = &configured_master_sensors[i];
        if (strncasecmp(sensor->function, "gps_", 4) != 0) {
            continue;
        }

        gps_signal_config_t *signal = &gps_signals[gps_signal_count++];
        signal->enabled = true;
        signal->preview_enabled = master_sensor_preview_enabled(sensor);
        signal->can_id = sensor->can_id;
        signal->sample_rate_hz = sensor->sample_rate_hz;
        copy_truncated(signal->function, sizeof(signal->function), sensor->function);

        if (gps_signal_count >= MAX_MASTER_SENSORS) {
            break;
        }
    }

    gps_configure_signals(gps_signals, gps_signal_count);
}

static void apply_rpm_signal_config(void)
{
    rpm_signal_config_t rpm_signals[MAX_MASTER_SENSORS] = {};
    size_t rpm_signal_count = 0;

    for (size_t i = 0; i < configured_master_sensor_count; i++) {
        const sensor_config_t *sensor = &configured_master_sensors[i];
        if (strcasecmp(sensor->function, "engine_rpm") != 0 &&
            strcasecmp(sensor->function, "rpm") != 0) {
            continue;
        }

        rpm_signal_config_t *signal = &rpm_signals[rpm_signal_count++];
        signal->enabled = true;
        signal->preview_enabled = master_sensor_preview_enabled(sensor);
        signal->can_id = sensor->can_id;
        signal->sample_rate_hz = sensor->sample_rate_hz;
        signal->gpio = sensor->port;
        copy_truncated(signal->function, sizeof(signal->function), sensor->function);

        if (rpm_signal_count >= MAX_MASTER_SENSORS) {
            break;
        }
    }

    rpm_sampler_configure_signals(rpm_signals, rpm_signal_count);
}

static void load_default_node_config(void)
{
    memset(configured_nodes, 0, sizeof(configured_nodes));
    configured_node_count = 2;
    configure_default_master_sensors();

    configure_default_brake_node(0, 1);
    configure_default_encoder_node(1, 4);

    apply_gps_signal_config();
    apply_rpm_signal_config();
    ESP_LOGW(TAG, "Using built-in fallback node config");
    node_config_loaded_from_sd = false;
}

static char *read_text_file(const char *path, size_t max_bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);
    if (file_size < 0 || (size_t)file_size > max_bytes) {
        fclose(file);
        ESP_LOGE(TAG, "Config file too large: %s", path);
        return NULL;
    }

    rewind(file);

    char *text = calloc(1, (size_t)file_size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }

    size_t read_len = fread(text, 1, (size_t)file_size, file);
    fclose(file);

    if (read_len != (size_t)file_size) {
        free(text);
        return NULL;
    }

    return text;
}

static esp_err_t load_node_config_from_sd(void)
{
    char *text = read_text_file(NODES_CONFIG_PATH, NODES_CONFIG_MAX_BYTES);
    if (!text) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", NODES_CONFIG_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "nodes_config.json is missing array field: nodes");
        return ESP_ERR_INVALID_ARG;
    }

    node_config_entry_t *parsed_nodes = calloc(MAX_CONFIGURED_NODES, sizeof(node_config_entry_t));
    if (!parsed_nodes) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No memory for parsed node config");
        return ESP_ERR_NO_MEM;
    }

    sensor_config_t parsed_master_sensors[MAX_MASTER_SENSORS] = {};
    size_t parsed_master_sensor_count = 0;
    size_t parsed_count = 0;

    const cJSON *master_sensors = cJSON_GetObjectItemCaseSensitive(root, "master_sensors");
    const cJSON *master_sensor_json = NULL;
    cJSON_ArrayForEach(master_sensor_json, master_sensors) {
        if (!cJSON_IsObject(master_sensor_json)) {
            continue;
        }

        if (!json_bool_or_default(cJSON_GetObjectItemCaseSensitive(master_sensor_json, "enabled"), true)) {
            continue;
        }

        if (parsed_master_sensor_count >= MAX_MASTER_SENSORS) {
            ESP_LOGW(TAG, "Ignoring extra master sensors beyond %u", MAX_MASTER_SENSORS);
            break;
        }

        const cJSON *sensor_id_json = cJSON_GetObjectItemCaseSensitive(master_sensor_json, "sensor_id");
        if (!cJSON_IsNumber(sensor_id_json)) {
            ESP_LOGW(TAG, "Skipping master sensor with missing/invalid sensor_id");
            continue;
        }

        sensor_config_t *sensor = &parsed_master_sensors[parsed_master_sensor_count];
        sensor->sensor_id = (uint8_t)sensor_id_json->valueint;
        const cJSON *sensor_name_json = cJSON_GetObjectItemCaseSensitive(master_sensor_json, "name");
        copy_truncated(sensor->name,
                       sizeof(sensor->name),
                       cJSON_IsString(sensor_name_json) ? sensor_name_json->valuestring : "master_sensor");
        sensor->can_id = parse_can_id_or_default(
            cJSON_GetObjectItemCaseSensitive(master_sensor_json, "can_id"),
            0x700 + parsed_master_sensor_count);
        const cJSON *units_json = cJSON_GetObjectItemCaseSensitive(master_sensor_json, "units");
        copy_truncated(sensor->units,
                       sizeof(sensor->units),
                       cJSON_IsString(units_json) ? units_json->valuestring : "raw");
        sensor->sample_rate_hz = json_u16_or_default(
            cJSON_GetObjectItemCaseSensitive(master_sensor_json, "sample_rate_hz"),
            1);
        const cJSON *function_json = cJSON_GetObjectItemCaseSensitive(master_sensor_json, "function");
        copy_truncated(sensor->function,
                       sizeof(sensor->function),
                       cJSON_IsString(function_json) ? function_json->valuestring : "gps_speed");
        sensor->port = parse_port_or_default(
            cJSON_GetObjectItemCaseSensitive(master_sensor_json, "port"),
            0);
        sensor->aux_port = parse_port_or_default(
            cJSON_GetObjectItemCaseSensitive(master_sensor_json, "aux_port"),
            0);
        sensor->preview_enabled = json_bool_or_default(
            cJSON_GetObjectItemCaseSensitive(master_sensor_json, "preview_enabled"),
            strcasecmp(sensor->function, "gps_speed") == 0);
        parsed_master_sensor_count++;
    }

    if (parsed_master_sensor_count == 0) {
        configure_default_master_sensors();
        memcpy(parsed_master_sensors,
               configured_master_sensors,
               sizeof(parsed_master_sensors));
        parsed_master_sensor_count = configured_master_sensor_count;
    }

    const cJSON *node_json = NULL;
    cJSON_ArrayForEach(node_json, nodes) {
        if (!cJSON_IsObject(node_json)) {
            continue;
        }

        if (!json_bool_or_default(cJSON_GetObjectItemCaseSensitive(node_json, "enabled"), true)) {
            continue;
        }

        if (parsed_count >= MAX_CONFIGURED_NODES) {
            ESP_LOGW(TAG, "Ignoring extra configured nodes beyond %u", MAX_CONFIGURED_NODES);
            break;
        }

        const cJSON *node_id_json = cJSON_GetObjectItemCaseSensitive(node_json, "node_id");
        if (!cJSON_IsNumber(node_id_json)) {
            ESP_LOGW(TAG, "Skipping node with missing/invalid node_id");
            continue;
        }

        node_config_entry_t *node = &parsed_nodes[parsed_count];
        node->node_id = (uint8_t)node_id_json->valueint;
        const cJSON *name_json = cJSON_GetObjectItemCaseSensitive(node_json, "name");
        copy_truncated(node->name,
                       sizeof(node->name),
                       cJSON_IsString(name_json) ? name_json->valuestring : "unnamed_node");
        node->active = json_bool_or_default(cJSON_GetObjectItemCaseSensitive(node_json, "active"), false);
        node->low_power_ack_seen = false;
        node->last_seen_rx_us = 0;
        node->last_state_rx_us = 0;
        node->last_recovery_attempt_us = 0;
        node->state_can_id = parse_can_id_or_default(
            cJSON_GetObjectItemCaseSensitive(node_json, "state_ack_can_id"),
            ID_NODE_STATE_BASE + node->node_id);

        const cJSON *sensors = cJSON_GetObjectItemCaseSensitive(node_json, "sensors");
        const cJSON *sensor_json = NULL;
        cJSON_ArrayForEach(sensor_json, sensors) {
            if (!cJSON_IsObject(sensor_json)) {
                continue;
            }

            if (!json_bool_or_default(cJSON_GetObjectItemCaseSensitive(sensor_json, "enabled"), true)) {
                continue;
            }

            if (node->sensor_count >= MAX_NODE_SENSORS) {
                ESP_LOGW(TAG, "Node %u has more than %u sensors; extra sensors ignored",
                         node->node_id,
                         MAX_NODE_SENSORS);
                break;
            }

            const cJSON *sensor_id_json = cJSON_GetObjectItemCaseSensitive(sensor_json, "sensor_id");
            if (!cJSON_IsNumber(sensor_id_json)) {
                ESP_LOGW(TAG, "Skipping sensor on node %u with missing/invalid sensor_id", node->node_id);
                continue;
            }

            sensor_config_t *sensor = &node->sensors[node->sensor_count];
            sensor->sensor_id = (uint8_t)sensor_id_json->valueint;
            const cJSON *sensor_name_json = cJSON_GetObjectItemCaseSensitive(sensor_json, "name");
            copy_truncated(sensor->name,
                           sizeof(sensor->name),
                           cJSON_IsString(sensor_name_json) ? sensor_name_json->valuestring : "unnamed_sensor");
            sensor->can_id = parse_can_id_or_default(
                cJSON_GetObjectItemCaseSensitive(sensor_json, "can_id"),
                parse_can_id_or_default(cJSON_GetObjectItemCaseSensitive(node_json, "data_can_id"), 0x0B1 + node->node_id));
            const cJSON *units_json = cJSON_GetObjectItemCaseSensitive(sensor_json, "units");
            copy_truncated(sensor->units,
                           sizeof(sensor->units),
                           cJSON_IsString(units_json) ? units_json->valuestring : "raw");
            sensor->sample_rate_hz = json_u16_or_default(
                cJSON_GetObjectItemCaseSensitive(sensor_json, "sample_rate_hz"),
                DEFAULT_SAMPLE_RATE_HZ);
            const cJSON *function_json = cJSON_GetObjectItemCaseSensitive(sensor_json, "function");
            copy_truncated(sensor->function,
                           sizeof(sensor->function),
                           cJSON_IsString(function_json) ? function_json->valuestring : "sim");
            sensor->port = parse_port_or_default(
                cJSON_GetObjectItemCaseSensitive(sensor_json, "port"),
                0);
            sensor->aux_port = parse_port_or_default(
                cJSON_GetObjectItemCaseSensitive(sensor_json, "aux_port"),
                0);
            sensor->preview_enabled = json_bool_or_default(
                cJSON_GetObjectItemCaseSensitive(sensor_json, "preview_enabled"),
                true);
            node->sensor_count++;
        }

        parsed_count++;
    }

    cJSON_Delete(root);

    if (parsed_count == 0 && parsed_master_sensor_count == 0) {
        ESP_LOGE(TAG, "No enabled nodes or master sensors found in %s", NODES_CONFIG_PATH);
        free(parsed_nodes);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(configured_nodes, parsed_nodes, MAX_CONFIGURED_NODES * sizeof(node_config_entry_t));
    free(parsed_nodes);
    memcpy(configured_master_sensors, parsed_master_sensors, sizeof(configured_master_sensors));
    configured_node_count = parsed_count;
    configured_master_sensor_count = parsed_master_sensor_count;
    node_config_loaded_from_sd = true;
    apply_gps_signal_config();
    apply_rpm_signal_config();
    ESP_LOGI(TAG,
             "Loaded %u node(s), %u master sensor(s) from %s",
             (unsigned)configured_node_count,
             (unsigned)configured_master_sensor_count,
             NODES_CONFIG_PATH);
    print_node_power_summary();
    return ESP_OK;
}

static esp_err_t build_current_node_config_text(char **out_text)
{
    if (!out_text) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_text = NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_CreateArray();
    cJSON *master_sensors = cJSON_CreateArray();
    if (!root || !nodes || !master_sensors) {
        cJSON_Delete(root);
        cJSON_Delete(nodes);
        cJSON_Delete(master_sensors);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddItemToObject(root, "master_sensors", master_sensors);
    cJSON_AddItemToObject(root, "nodes", nodes);

    for (size_t s = 0; s < configured_master_sensor_count; s++) {
        const sensor_config_t *sensor = &configured_master_sensors[s];
        cJSON *sensor_json = cJSON_CreateObject();
        if (!sensor_json) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        char can_id[12];
        snprintf(can_id, sizeof(can_id), "0x%03" PRIX32, sensor->can_id);

        cJSON_AddNumberToObject(sensor_json, "sensor_id", sensor->sensor_id);
        cJSON_AddStringToObject(sensor_json, "name", sensor->name);
        cJSON_AddBoolToObject(sensor_json, "enabled", true);
        cJSON_AddStringToObject(sensor_json, "can_id", can_id);
        cJSON_AddStringToObject(sensor_json, "units", sensor->units);
        cJSON_AddNumberToObject(sensor_json, "sample_rate_hz", sensor->sample_rate_hz);
        cJSON_AddStringToObject(sensor_json, "function",
                                sensor->function[0] ? sensor->function : "gps_speed");
        cJSON_AddNumberToObject(sensor_json, "port", sensor->port);
        cJSON_AddNumberToObject(sensor_json, "aux_port", sensor->aux_port);
        cJSON_AddBoolToObject(sensor_json,
                              "preview_enabled",
                              master_sensor_preview_enabled(sensor));
        cJSON_AddItemToArray(master_sensors, sensor_json);
    }

    for (size_t i = 0; i < configured_node_count; i++) {
        const node_config_entry_t *node = &configured_nodes[i];
        cJSON *node_json = cJSON_CreateObject();
        cJSON *sensors = cJSON_CreateArray();
        if (!node_json || !sensors) {
            cJSON_Delete(node_json);
            cJSON_Delete(sensors);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        char state_can_id[12];
        snprintf(state_can_id, sizeof(state_can_id), "0x%03" PRIX32, node->state_can_id);

        cJSON_AddNumberToObject(node_json, "node_id", node->node_id);
        cJSON_AddStringToObject(node_json, "name", node->name);
        cJSON_AddBoolToObject(node_json, "enabled", true);
        cJSON_AddBoolToObject(node_json, "active", node->active);
        cJSON_AddStringToObject(node_json, "state_ack_can_id", state_can_id);
        cJSON_AddItemToObject(node_json, "sensors", sensors);

        for (size_t s = 0; s < node->sensor_count; s++) {
            const sensor_config_t *sensor = &node->sensors[s];
            cJSON *sensor_json = cJSON_CreateObject();
            if (!sensor_json) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }

            char can_id[12];
            snprintf(can_id, sizeof(can_id), "0x%03" PRIX32, sensor->can_id);

            cJSON_AddNumberToObject(sensor_json, "sensor_id", sensor->sensor_id);
            cJSON_AddStringToObject(sensor_json, "name", sensor->name);
            cJSON_AddBoolToObject(sensor_json, "enabled", true);
            cJSON_AddStringToObject(sensor_json, "can_id", can_id);
            cJSON_AddStringToObject(sensor_json, "units", sensor->units);
            cJSON_AddNumberToObject(sensor_json, "sample_rate_hz", sensor->sample_rate_hz);
            cJSON_AddStringToObject(sensor_json, "function",
                                    sensor->function[0] ? sensor->function : "sim");
            cJSON_AddNumberToObject(sensor_json, "port", sensor->port);
            cJSON_AddNumberToObject(sensor_json, "aux_port", sensor->aux_port);
            cJSON_AddItemToArray(sensors, sensor_json);
        }

        cJSON_AddItemToArray(nodes, node_json);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }

    *out_text = text;
    return ESP_OK;
}

esp_err_t master_get_node_config_text(char **out_text, bool *out_from_sd)
{
    if (!out_text) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_text = NULL;
    if (out_from_sd) {
        *out_from_sd = false;
    }

    char *text = read_text_file(NODES_CONFIG_PATH, NODES_CONFIG_MAX_BYTES);
    if (text) {
        *out_text = text;
        if (out_from_sd) {
            *out_from_sd = true;
        }
        return ESP_OK;
    }

    if (create_node_config_file_from_current() == ESP_OK) {
        text = read_text_file(NODES_CONFIG_PATH, NODES_CONFIG_MAX_BYTES);
        if (text) {
            *out_text = text;
            if (out_from_sd) {
                *out_from_sd = true;
            }
            return ESP_OK;
        }
    }

    return build_current_node_config_text(out_text);
}

esp_err_t master_reload_node_config(void)
{
    if (log_state != LOG_STATE_IDLE) {
        ESP_LOGW(TAG, "Config reload ignored; logger is %s", log_state_name(log_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = load_node_config_from_sd();
    if (err == ESP_ERR_NOT_FOUND) {
        err = create_node_config_file_from_current();
        if (err == ESP_OK) {
            err = load_node_config_from_sd();
        }
    }

    return err;
}

static esp_err_t validate_node_config_text(const char *text, size_t len)
{
    if (!text || len == 0 || len > NODES_CONFIG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t write_text_file(const char *path, const char *text, size_t len)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open %s for writing: errno=%d (%s)",
                 path,
                 errno,
                 strerror(errno));
        return ESP_FAIL;
    }

    size_t written = fwrite(text, 1, len, file);
    int close_err = fclose(file);

    if (written != len || close_err != 0) {
        ESP_LOGE(TAG,
                 "Failed to write %s: wrote=%u expected=%u close_err=%d errno=%d (%s)",
                 path,
                 (unsigned)written,
                 (unsigned)len,
                 close_err,
                 errno,
                 strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool is_safe_mqtt_uri(const char *uri)
{
    if (!uri) {
        return false;
    }

    size_t len = strlen(uri);
    if (len == 0 || len > 120) {
        return false;
    }

    if (strncmp(uri, "mqtt://", 7) != 0 && strncmp(uri, "mqtts://", 8) != 0) {
        return false;
    }

    for (const char *p = uri; *p; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            return false;
        }
    }

    return true;
}

static esp_err_t save_mqtt_broker_uri(const char *uri)
{
    if (!is_safe_mqtt_uri(uri)) {
        return ESP_ERR_INVALID_ARG;
    }

    char json[MQTT_CONFIG_MAX_BYTES];
    int len = snprintf(json, sizeof(json), "{\n  \"broker_uri\": \"%s\"\n}\n", uri);
    if (len < 0 || len >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = write_text_file(MQTT_CONFIG_TMP_PATH, json, (size_t)len);
    if (err != ESP_OK) {
        return err;
    }

    remove(MQTT_CONFIG_PATH);
    if (rename(MQTT_CONFIG_TMP_PATH, MQTT_CONFIG_PATH) != 0) {
        remove(MQTT_CONFIG_TMP_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved MQTT broker config to %s", MQTT_CONFIG_PATH);
    return ESP_OK;
}

static esp_err_t create_node_config_file_from_current(void)
{
    struct stat st;
    if (stat(NODES_CONFIG_PATH, &st) == 0) {
        return ESP_OK;
    }

    char *text = NULL;
    esp_err_t err = build_current_node_config_text(&text);
    if (err != ESP_OK) {
        return err;
    }

    err = write_text_file(NODES_CONFIG_TMP_PATH, text, strlen(text));
    free(text);
    if (err != ESP_OK) {
        remove(NODES_CONFIG_TMP_PATH);
        ESP_LOGE(TAG, "Failed to create temp default node config");
        return err;
    }

    if (rename(NODES_CONFIG_TMP_PATH, NODES_CONFIG_PATH) != 0) {
        remove(NODES_CONFIG_TMP_PATH);
        ESP_LOGE(TAG, "Failed to create %s", NODES_CONFIG_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created default node config at %s", NODES_CONFIG_PATH);
    return ESP_OK;
}

esp_err_t master_save_node_config_text(const char *text, size_t len, bool apply_now)
{
    if (log_state != LOG_STATE_IDLE) {
        ESP_LOGW(TAG, "Config save ignored; logger is %s", log_state_name(log_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = validate_node_config_text(text, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Rejected invalid node config: %s", esp_err_to_name(err));
        return err;
    }

    err = write_text_file(NODES_CONFIG_TMP_PATH, text, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write temp node config");
        return err;
    }

    struct stat st;
    bool had_existing = (stat(NODES_CONFIG_PATH, &st) == 0);

    remove(NODES_CONFIG_BAK_PATH);
    if (had_existing && rename(NODES_CONFIG_PATH, NODES_CONFIG_BAK_PATH) != 0) {
        remove(NODES_CONFIG_TMP_PATH);
        ESP_LOGE(TAG, "Failed to back up existing node config");
        return ESP_FAIL;
    }

    if (rename(NODES_CONFIG_TMP_PATH, NODES_CONFIG_PATH) != 0) {
        remove(NODES_CONFIG_TMP_PATH);
        if (had_existing) {
            (void)rename(NODES_CONFIG_BAK_PATH, NODES_CONFIG_PATH);
        }
        ESP_LOGE(TAG, "Failed to install new node config");
        return ESP_FAIL;
    }

    if (apply_now) {
        err = load_node_config_from_sd();
        if (err != ESP_OK) {
            remove(NODES_CONFIG_PATH);
            if (had_existing) {
                (void)rename(NODES_CONFIG_BAK_PATH, NODES_CONFIG_PATH);
                (void)load_node_config_from_sd();
            }
            ESP_LOGE(TAG, "New node config failed to load; rolled back");
            return err;
        }
    }

    remove(NODES_CONFIG_BAK_PATH);
    ESP_LOGI(TAG, "Saved node config to %s", NODES_CONFIG_PATH);
    return ESP_OK;
}

/* --------------------- TWAI RX ------------------ */
static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8] = {0};
    twai_frame_t frame = {};
    frame.buffer     = raw;
    frame.buffer_len = sizeof(raw);

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {
        can_rx_word_t msg = {};
        msg.id = frame.header.id;

        uint8_t dlc = frame.header.dlc;
        if (dlc > 8) dlc = 8;
        msg.dlc = dlc;
        record_can_rx_frame(dlc);

        uint64_t packed = 0;
        for (int i = 0; i < msg.dlc; i++) {
            packed |= (uint64_t)raw[i] << (8 * i); 
        }
        msg.data = packed;

        (void)xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

static twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

/* --------------------- Log file helpers ------------------ */

// Finds the next unused /sdcard/log_XXXX.bin path for a new run.
static void create_unique_log_filename(char *out_path, size_t max_len)
{
    int index = 1;
    while (1) {
        snprintf(out_path, max_len, "/sdcard/log_%04d.bin", index);
        struct stat st;
        if (stat(out_path, &st) != 0) return;
        index++;
    }
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

static const char *log_state_name(log_state_t state)
{
    switch (state) {
    case LOG_STATE_IDLE:
        return "idle";
    case LOG_STATE_RUNNING:
        return "running";
    case LOG_STATE_STOPPING:
        return "stopping";
    default:
        return "unknown";
    }
}

/* --------------------- Master -> node CAN commands ------------------ */

/*
 * Start does more than send ID_START_CMD:
 *   1. Reset each node runtime sensor config.
 *   2. Send enabled sensor ID/CAN ID/sample-rate entries.
 *   3. Send targeted START frames only to enabled nodes.
 *
 * This lets the dashboard edit nodes_config.json on the master, then push the
 * active runtime config to nodes without reflashing their firmware.
 */
static esp_err_t send_control_frame(uint32_t id, const uint8_t *payload, uint8_t len)
{
    if (!master_can_ready()) {
        ESP_LOGD(TAG,
                 "CAN disabled/not ready; skipped TX CMD | ID: 0x%03" PRIX32 " len=%u",
                 id,
                 len);
        return ESP_ERR_INVALID_STATE;
    }

    twai_frame_t tx = {
        .header.id = id,
        .header.ide = false,
        .buffer = (uint8_t *)payload,
        .buffer_len = len,
    };

    esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        record_can_tx_frame(len);
        ESP_LOGI(TAG, "TX CMD | ID: 0x%03" PRIX32 " len=%u", id, len);
    } else {
        ESP_LOGW(TAG,
                 "TX CMD failed | ID: 0x%03" PRIX32 " len=%u err=%s",
                 id,
                 len,
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t send_control_command(uint32_t id)
{
    return send_control_frame(id, NULL, 0);
}

static esp_err_t send_targeted_control_command(uint32_t id, uint8_t node_id)
{
    uint8_t payload[1] = {node_id};
    return send_control_frame(id, payload, sizeof(payload));
}

static esp_err_t send_node_config_reset(uint8_t node_id)
{
    uint8_t payload[8] = {
        node_id,
        NODE_CONFIG_CMD_RESET,
    };

    return send_control_frame(ID_NODE_CONFIG_CMD, payload, sizeof(payload));
}

static esp_err_t send_sensor_runtime_config(uint8_t node_id, const sensor_config_t *sensor)
{
    if (!sensor) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t can_id = (uint16_t)(sensor->can_id & 0x7FFu);
    uint16_t sample_rate_hz = sensor->sample_rate_hz > 0 ?
                             sensor->sample_rate_hz :
                             DEFAULT_SAMPLE_RATE_HZ;
    uint8_t payload[8] = {
        node_id,
        NODE_CONFIG_CMD_SENSOR,
        sensor->sensor_id,
        1,
        (uint8_t)(sample_rate_hz & 0xFFu),
        (uint8_t)((sample_rate_hz >> 8) & 0xFFu),
        (uint8_t)(can_id & 0xFFu),
        (uint8_t)((can_id >> 8) & 0xFFu),
    };

    return send_control_frame(ID_NODE_CONFIG_CMD, payload, sizeof(payload));
}

static esp_err_t send_sensor_io_runtime_config(uint8_t node_id, const sensor_config_t *sensor)
{
    if (!sensor) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[8] = {
        node_id,
        NODE_CONFIG_CMD_SENSOR_IO,
        sensor->sensor_id,
        sensor_function_code(sensor->function),
        sensor->port,
        sensor->aux_port,
        0,
        0,
    };

    return send_control_frame(ID_NODE_CONFIG_CMD, payload, sizeof(payload));
}

static esp_err_t send_node_log_mode(uint8_t node_id, uint8_t node_log_mode)
{
    uint8_t payload[8] = {
        node_id,
        NODE_CONFIG_CMD_LOG,
        node_log_mode,
    };

    return send_control_frame(ID_NODE_CONFIG_CMD, payload, sizeof(payload));
}

esp_err_t master_set_log_mode_text(const char *mode_text)
{
    bool ok = false;
    uint8_t mode = parse_log_mode_text(mode_text, &ok);
    if (!ok) {
        ESP_LOGW(TAG,
                 "Unknown log mode '%s'. Use off, master, status, node, samples, or all.",
                 mode_text ? mode_text : "");
        return ESP_ERR_INVALID_ARG;
    }

    runtime_log_mode = mode;

    uint8_t node_log_mode = 0;
    if (mode & LOG_MODE_NODE_STATUS) {
        node_log_mode |= NODE_LOG_MODE_STATUS;
    }
    if (mode & LOG_MODE_NODE_SAMPLES) {
        node_log_mode |= NODE_LOG_MODE_SAMPLES;
    }

    (void)send_node_log_mode(0xFF, node_log_mode);
    ESP_LOGI(TAG,
             "Log mode set to %s (master_samples=%s node_status=%s node_samples=%s)",
             master_log_mode_name(mode),
             (mode & LOG_MODE_MASTER_SAMPLES) ? "on" : "off",
             (mode & LOG_MODE_NODE_STATUS) ? "on" : "off",
             (mode & LOG_MODE_NODE_SAMPLES) ? "on" : "off");
    return ESP_OK;
}

static void send_runtime_config_and_start_node(node_config_entry_t *node)
{
    if (!node) {
        return;
    }

    (void)send_targeted_control_command(ID_STOP_CMD, node->node_id);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)send_node_config_reset(node->node_id);
    vTaskDelay(pdMS_TO_TICKS(10));

    for (size_t s = 0; s < node->sensor_count; s++) {
        const sensor_config_t *sensor = &node->sensors[s];
        ESP_LOGI(TAG,
                 "Config node=%u sensor=%u %s can_id=0x%03" PRIX32
                 " rate=%uHz function=%s port=%u aux_port=%u",
                 node->node_id,
                 sensor->sensor_id,
                 sensor->name,
                 sensor->can_id,
                 sensor->sample_rate_hz,
                 sensor->function[0] ? sensor->function : "sim",
                 sensor->port,
                 sensor->aux_port);
        (void)send_sensor_runtime_config(node->node_id, sensor);
        (void)send_sensor_io_runtime_config(node->node_id, sensor);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (node->sensor_count > 0) {
        (void)send_targeted_control_command(ID_START_CMD, node->node_id);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_LOGI(TAG, "Node %u has no enabled sensors; leaving stopped", node->node_id);
    }
}

static void send_runtime_config_and_start_nodes(void)
{
    ESP_LOGI(TAG, "Sending runtime node configuration");
    for (size_t i = 0; i < configured_node_count; i++) {
        send_runtime_config_and_start_node(&configured_nodes[i]);
    }
}

static node_config_entry_t *find_configured_node_by_state_can_id(uint32_t can_id)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        if (configured_nodes[i].state_can_id == can_id) {
            return &configured_nodes[i];
        }
    }

    return NULL;
}

static node_config_entry_t *find_configured_node_by_node_id(uint8_t node_id)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        if (configured_nodes[i].node_id == node_id) {
            return &configured_nodes[i];
        }
    }

    return NULL;
}

static node_config_entry_t *find_configured_node_by_sensor_can_id(uint32_t can_id)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        for (size_t s = 0; s < configured_nodes[i].sensor_count; s++) {
            if (configured_nodes[i].sensors[s].can_id == can_id) {
                return &configured_nodes[i];
            }
        }
    }

    return NULL;
}

static void mark_node_seen_by_frame(const can_rx_word_t *msg)
{
    node_config_entry_t *node = find_configured_node_by_state_can_id(msg->id);
    if (!node) {
        node = find_configured_node_by_sensor_can_id(msg->id);
    }
    if (!node && msg->id >= ID_NODE_HEALTH_BASE && msg->id < (ID_NODE_HEALTH_BASE + 0x40)) {
        node = find_configured_node_by_node_id((uint8_t)(msg->id - ID_NODE_HEALTH_BASE));
    }

    if (node) {
        node->last_seen_rx_us = esp_timer_get_time();
    }
}

static void mark_all_low_power_acks_pending(void)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        configured_nodes[i].low_power_ack_seen = false;
    }
}

/* --------------------- Node state tracking ------------------ */

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

static const char *node_reset_reason_name(uint8_t reset_reason)
{
    switch (reset_reason) {
    case 1:
        return "poweron";
    case 2:
        return "external";
    case 3:
        return "software";
    case 4:
        return "panic";
    case 5:
        return "interrupt_wdt";
    case 6:
        return "task_wdt";
    case 7:
        return "watchdog";
    case 8:
        return "deepsleep";
    case 9:
        return "brownout";
    case 10:
        return "sdio";
    case 11:
        return "usb";
    case 12:
        return "jtag";
    case 13:
        return "efuse";
    case 14:
        return "power_glitch";
    case 15:
        return "cpu_lockup";
    default:
        return "unknown";
    }
}

static void print_node_power_summary(void)
{
    ESP_LOGI(TAG, "Configured node power status:");
    for (size_t i = 0; i < configured_node_count; i++) {
        const node_config_entry_t *node = &configured_nodes[i];
        const char *state = node->active ? "active" : "low-power/offline";
        const char *stop_ack = node->low_power_ack_seen ? "stop-ack=yes" : "stop-ack=no";

        ESP_LOGI(TAG,
                 "  node_id=%u name=%s state=%s %s state_can_id=0x%03" PRIX32
                 " last_seen_rx_us=%lld last_state_rx_us=%lld",
                 node->node_id,
                 node->name,
                 state,
                 stop_ack,
                 node->state_can_id,
                 (long long)node->last_seen_rx_us,
                 (long long)node->last_state_rx_us);

        for (size_t s = 0; s < node->sensor_count; s++) {
            const sensor_config_t *sensor = &node->sensors[s];
            ESP_LOGI(TAG,
                     "    sensor_id=%u name=%s can_id=0x%03" PRIX32
                     " units=%s sample_rate_hz=%u function=%s port=%u aux_port=%u",
                     sensor->sensor_id,
                     sensor->name,
                     sensor->can_id,
                     sensor->units,
                     sensor->sample_rate_hz,
                     sensor->function[0] ? sensor->function : "sim",
                     sensor->port,
                     sensor->aux_port);
        }
    }
}

static void handle_node_state_frame(const can_rx_word_t *msg)
{
    node_config_entry_t *node = find_configured_node_by_state_can_id(msg->id);
    uint8_t state = (uint8_t)(msg->data & 0xFFu);
    uint8_t reason = msg->dlc >= 2 ? (uint8_t)((msg->data >> 8) & 0xFFu) : 0;
    uint8_t reset_reason = msg->dlc >= 3 ? (uint8_t)((msg->data >> 16) & 0xFFu) : 0;

    if (!node) {
        ESP_LOGW(TAG,
                 "State ACK from unconfigured node frame 0x%03" PRIX32
                 " state=%u reason=%s reset=%s(%u) dlc=%u data=0x%016" PRIX64,
                 msg->id,
                 state,
                 node_state_reason_name(reason),
                 node_reset_reason_name(reset_reason),
                 reset_reason,
                 msg->dlc,
                 msg->data);
        return;
    }

    node->last_state_rx_us = esp_timer_get_time();
    node->last_seen_rx_us = node->last_state_rx_us;

    if (state == NODE_STATE_ACTIVE) {
        node->active = true;
        node->low_power_ack_seen = false;
        ESP_LOGI(TAG,
                 "Node %u (%s) confirmed ACTIVE reason=%s reset=%s(%u) dlc=%u data=0x%016" PRIX64,
                 node->node_id,
                 node->name,
                 node_state_reason_name(reason),
                 node_reset_reason_name(reset_reason),
                 reset_reason,
                 msg->dlc,
                 msg->data);
    } else if (state == NODE_STATE_LOW_POWER) {
        node->active = false;
        node->low_power_ack_seen = true;
        ESP_LOGI(TAG,
                 "Node %u (%s) confirmed LOW-POWER/OFF reason=%s reset=%s(%u) dlc=%u data=0x%016" PRIX64,
                 node->node_id,
                 node->name,
                 node_state_reason_name(reason),
                 node_reset_reason_name(reset_reason),
                 reset_reason,
                 msg->dlc,
                 msg->data);
    } else {
        ESP_LOGW(TAG,
                 "Node %u (%s) sent unknown state=%u reason=%s reset=%s(%u) dlc=%u data=0x%016" PRIX64,
                 node->node_id,
                 node->name,
                 state,
                 node_state_reason_name(reason),
                 node_reset_reason_name(reset_reason),
                 reset_reason,
                 msg->dlc,
                 msg->data);
    }
}

static void handle_node_health_frame(const can_rx_word_t *msg)
{
    if (msg->dlc < 2 || msg->id < ID_NODE_HEALTH_BASE ||
        msg->id >= (ID_NODE_HEALTH_BASE + 0x40)) {
        return;
    }

    node_config_entry_t *node =
        find_configured_node_by_node_id((uint8_t)(msg->id - ID_NODE_HEALTH_BASE));
    if (!node) {
        return;
    }

    bool reported_active = ((msg->data >> 8) & 0x01u) != 0;
    if (node->active && !reported_active && log_state == LOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Node %u (%s) reports idle while logging", node->node_id, node->name);
    }
    node->active = reported_active;
}

/* --------------------- Public control API ------------------ */

bool master_start_logging(void)
{
    if (log_state != LOG_STATE_IDLE) {
        ESP_LOGW(TAG, "Start ignored; logger is %s", log_state_name(log_state));
        return false;
    }

    create_unique_log_filename(current_log_path, sizeof(current_log_path));

    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        return false;
    }

    close_log_file_if_open();
    logFile = fopen(current_log_path, "wb");
    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Failed to open log file: %s", current_log_path);
        return false;
    }
    xSemaphoreGive(log_file_mutex);

    xQueueReset(twai_rx_sample_queue);
    writeCounter = 0;
    log_state = LOG_STATE_RUNNING;
    gps_set_logging_enabled(true);

    int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < configured_node_count; i++) {
        configured_nodes[i].active = false;
        configured_nodes[i].low_power_ack_seen = false;
        configured_nodes[i].last_seen_rx_us = start_us;
        configured_nodes[i].last_recovery_attempt_us = start_us;
    }

    ESP_LOGI(TAG, "Logging started: %s", current_log_path);
    send_runtime_config_and_start_nodes();
    return true;
}

bool master_stop_logging(void)
{
    if (log_state != LOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Stop ignored; logger is %s", log_state_name(log_state));
        return false;
    }

    log_state = LOG_STATE_STOPPING;
    gps_set_logging_enabled(false);
    ESP_LOGI(TAG, "Logging stop requested; draining queued samples...");
    mark_all_low_power_acks_pending();
    for (size_t i = 0; i < configured_node_count; i++) {
        (void)send_targeted_control_command(ID_STOP_CMD, configured_nodes[i].node_id);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(NODE_LOW_POWER_ACK_WAIT_MS));
    print_node_power_summary();
    return true;
}

bool master_submit_local_sample(uint32_t can_id,
                                uint32_t value,
                                uint32_t timestamp_ms,
                                bool preview_enabled)
{
    can_rx_word_t msg = {
        .id = can_id,
        .dlc = 8,
        .data = ((uint64_t)timestamp_ms << 32) | value,
    };

    if (preview_enabled) {
        telemetry_submit_can_frame(msg.id, msg.dlc, msg.data);
    }

    if (log_state != LOG_STATE_RUNNING || !twai_rx_sample_queue) {
        return true;
    }

    if (xQueueSend(twai_rx_sample_queue, &msg, 1) != pdPASS) {
        rx_sample_queue_drops++;
        ESP_LOGW(TAG, "Sample queue FULL - local sample dropped");
        return false;
    }

    return true;
}

/* --------------------- Local serial status/files helpers ------------------ */

static void print_sd_card_status(void)
{
    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) == pdTRUE) {
        if (logFile) {
            fflush(logFile);
        }
        xSemaphoreGive(log_file_mutex);
    }

    ESP_LOGI(TAG, "SD files:");

    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open /sdcard");
    } else {
        struct dirent *entry;
        int file_count = 0;

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char path[LOG_PATH_MAX_LEN + 32];
            int needed = snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
            if (needed < 0 || needed >= (int)sizeof(path)) {
                ESP_LOGW(TAG, "Skipping long filename: %s", entry->d_name);
                continue;
            }

            struct stat st;
            if (stat(path, &st) == 0) {
                ESP_LOGI(TAG, "  %s | %lld bytes", entry->d_name, (long long)st.st_size);
                file_count++;
            } else {
                ESP_LOGW(TAG, "  %s | size unavailable", entry->d_name);
            }
        }

        closedir(dir);

        if (file_count == 0) {
            ESP_LOGI(TAG, "  (empty)");
        }
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT fr = f_getfree("/sdcard", &free_clusters, &fs);
    if (fr == FR_OK && fs != NULL) {
        uint64_t total_clusters = (uint64_t)(fs->n_fatent - 2);
        uint64_t sectors_per_cluster = (uint64_t)fs->csize;
        uint64_t bytes_per_sector = FF_MIN_SS;
        uint64_t total_bytes = total_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t free_bytes = (uint64_t)free_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t used_bytes = total_bytes - free_bytes;
        uint64_t free_pct_x10 = total_bytes > 0
                              ? (free_bytes * 1000ULL) / total_bytes
                              : 0;
        uint64_t used_pct_x10 = total_bytes > 0
                              ? (used_bytes * 1000ULL) / total_bytes
                              : 0;

        double used_gb = (double)used_bytes / 1000000000.0;
        double free_gb = (double)free_bytes / 1000000000.0;
        double total_gb = (double)total_bytes / 1000000000.0;

        ESP_LOGI(TAG,
                 "SD space: used=%.2f GB (%llu.%01llu%%) free=%.2f GB (%llu.%01llu%%) total=%.2f GB",
                 used_gb,
                 (unsigned long long)(used_pct_x10 / 10),
                 (unsigned long long)(used_pct_x10 % 10),
                 free_gb,
                 (unsigned long long)(free_pct_x10 / 10),
                 (unsigned long long)(free_pct_x10 % 10),
                 total_gb);
    } else {
        ESP_LOGE(TAG, "Failed to read SD free/used space: FatFS result=%d", (int)fr);
    }
}

/* --------------------- JSON helpers used by telemetry.c ------------------ */

static int append_json(char *out, size_t out_size, size_t *used, const char *fmt, ...)
{
    if (*used >= out_size) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *used, out_size - *used, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= out_size - *used) {
        return -1;
    }

    *used += (size_t)written;
    return 0;
}

esp_err_t master_format_files_json(char *out, size_t out_size)
{
    size_t used = 0;
    if (append_json(out, out_size, &used, "{\"ok\":true,\"files\":[") != 0) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) == pdTRUE) {
        if (logFile) {
            fflush(logFile);
        }
        xSemaphoreGive(log_file_mutex);
    }

    DIR *dir = opendir("/sdcard");
    bool first = true;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char path[LOG_PATH_MAX_LEN + 32];
            int needed = snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
            if (needed < 0 || needed >= (int)sizeof(path)) {
                continue;
            }

            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }

            if (append_json(out,
                            out_size,
                            &used,
                            "%s{\"name\":\"%s\",\"size_bytes\":%lld}",
                            first ? "" : ",",
                            entry->d_name,
                            (long long)st.st_size) != 0) {
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }
            first = false;
        }
        closedir(dir);
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT fr = f_getfree("/sdcard", &free_clusters, &fs);
    if (fr == FR_OK && fs != NULL) {
        uint64_t total_clusters = (uint64_t)(fs->n_fatent - 2);
        uint64_t sectors_per_cluster = (uint64_t)fs->csize;
        uint64_t bytes_per_sector = FF_MIN_SS;
        uint64_t total_bytes = total_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t free_bytes = (uint64_t)free_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t used_bytes = total_bytes - free_bytes;
        double used_gb = (double)used_bytes / 1000000000.0;
        double free_gb = (double)free_bytes / 1000000000.0;
        double total_gb = (double)total_bytes / 1000000000.0;
        double used_percent = total_bytes > 0 ? ((double)used_bytes * 100.0) / (double)total_bytes : 0.0;
        double free_percent = total_bytes > 0 ? ((double)free_bytes * 100.0) / (double)total_bytes : 0.0;

        if (append_json(out,
                        out_size,
                        &used,
                        "],\"sd\":{\"used_gb\":%.2f,\"free_gb\":%.2f,\"total_gb\":%.2f,"
                        "\"used_percent\":%.1f,\"free_percent\":%.1f}}",
                        used_gb,
                        free_gb,
                        total_gb,
                        used_percent,
                        free_percent) != 0) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        if (append_json(out,
                        out_size,
                        &used,
                        "],\"sd\":{\"error\":\"f_getfree_failed\",\"fatfs_result\":%d}}",
                        (int)fr) != 0) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static unsigned queue_depth_or_zero(QueueHandle_t queue)
{
    if (!queue) {
        return 0;
    }

    return (unsigned)uxQueueMessagesWaiting(queue);
}

static uint32_t percent_clamped_u32(uint32_t value, uint32_t max_value)
{
    if (max_value == 0) {
        return 0;
    }

    uint32_t percent = (value * 100u) / max_value;
    return percent > 100u ? 100u : percent;
}

esp_err_t master_format_health_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_us = esp_timer_get_time();
    uint32_t free_heap_kb = esp_get_free_heap_size() / 1024u;
    unsigned rx_depth = queue_depth_or_zero(twai_rx_queue);
    unsigned sample_depth = queue_depth_or_zero(twai_rx_sample_queue);
    unsigned heartbeat_depth = queue_depth_or_zero(twai_rx_heartbeat_queue);
    uint32_t last_write_ms = sd_write_last_ms;
    uint32_t max_write_ms = sd_write_max_ms;
    uint32_t rx_load = percent_clamped_u32(rx_depth, RX_QUEUE_LENGTH);
    uint32_t sample_load = percent_clamped_u32(sample_depth, RX_QUEUE_LENGTH);
    uint32_t heartbeat_load = percent_clamped_u32(heartbeat_depth, RX_QUEUE_LENGTH);
    uint32_t sd_load = percent_clamped_u32(last_write_ms, 250u);
    uint32_t load_percent = rx_load;
    if (sample_load > load_percent) {
        load_percent = sample_load;
    }
    if (heartbeat_load > load_percent) {
        load_percent = heartbeat_load;
    }
    if (sd_load > load_percent) {
        load_percent = sd_load;
    }

    uint32_t can_rx_frames = 0;
    uint32_t can_rx_bits = 0;
    uint32_t can_tx_frames = 0;
    uint32_t can_tx_bits = 0;
    uint32_t dispatch_busy_us = 0;
    uint32_t sd_writer_busy_us = 0;

    portENTER_CRITICAL(&health_stats_lock);
    can_rx_frames = can_rx_frames_interval;
    can_rx_bits = can_rx_bits_interval;
    can_tx_frames = can_tx_frames_interval;
    can_tx_bits = can_tx_bits_interval;
    dispatch_busy_us = dispatch_busy_us_interval;
    sd_writer_busy_us = sd_writer_busy_us_interval;
    can_rx_frames_interval = 0;
    can_rx_bits_interval = 0;
    can_tx_frames_interval = 0;
    can_tx_bits_interval = 0;
    dispatch_busy_us_interval = 0;
    sd_writer_busy_us_interval = 0;
    portEXIT_CRITICAL(&health_stats_lock);

    int64_t elapsed_us = health_last_report_us > 0
                       ? now_us - health_last_report_us
                       : 1000000;
    health_last_report_us = now_us;
    if (elapsed_us <= 0) {
        elapsed_us = 1000000;
    }

    uint64_t total_task_busy_us = (uint64_t)dispatch_busy_us + (uint64_t)sd_writer_busy_us;
    uint32_t task_load_percent = (uint32_t)((total_task_busy_us * 100u) / (uint64_t)elapsed_us);
    if (task_load_percent > 100u) {
        task_load_percent = 100u;
    }

    uint32_t can_total_bits = can_rx_bits + can_tx_bits;
    uint32_t can_bits_per_sec = (uint32_t)(((uint64_t)can_total_bits * 1000000u) / (uint64_t)elapsed_us);
    uint32_t can_bus_load_x100 = (uint32_t)(((uint64_t)can_bits_per_sec * 10000u) / TRANSM_RATE);
    uint32_t can_rx_fps_x100 = (uint32_t)(((uint64_t)can_rx_frames * 100000000u) / (uint64_t)elapsed_us);
    uint32_t can_tx_fps_x100 = (uint32_t)(((uint64_t)can_tx_frames * 100000000u) / (uint64_t)elapsed_us);
    uint32_t can_total_fps_x100 = can_rx_fps_x100 + can_tx_fps_x100;

    int len = snprintf(out,
                       out_size,
                       "{\"source\":\"master\",\"node_id\":0,\"name\":\"master\","
                       "\"active\":%s,\"logger_state\":\"%s\",\"can_enabled\":%s,"
                       "\"load_percent\":%" PRIu32 ",\"task_load_percent\":%" PRIu32 ","
                       "\"cpu_load_percent\":%" PRIu32 ",\"pressure_percent\":%" PRIu32 ","
                       "\"rx_queue_depth\":%u,\"sample_queue_depth\":%u,"
                       "\"heartbeat_queue_depth\":%u,\"sample_queue_drops\":%" PRIu32 ","
                       "\"heartbeat_queue_drops\":%" PRIu32 ",\"sd_write_last_ms\":%" PRIu32 ","
                       "\"sd_write_max_ms\":%" PRIu32 ",\"sd_write_failures\":%" PRIu32 ","
                       "\"rx_queue_capacity\":%u,\"sample_queue_capacity\":%u,"
                       "\"heartbeat_queue_capacity\":%u,"
                       "\"can_bitrate_bps\":%u,\"can_bus_load_x100\":%" PRIu32 ","
                       "\"can_bits_per_sec\":%" PRIu32 ","
                       "\"can_frames_per_sec_x100\":%" PRIu32 ","
                       "\"can_rx_frames_per_sec_x100\":%" PRIu32 ","
                       "\"can_tx_frames_per_sec_x100\":%" PRIu32 ","
                       "\"can_rx_frames\":%" PRIu32 ",\"can_tx_frames\":%" PRIu32 ","
                       "\"free_heap_kb\":%" PRIu32 ",\"blocks\":%d,"
                       "\"publish_time_us\":%" PRId64 "}",
                       log_state == LOG_STATE_RUNNING ? "true" : "false",
                       log_state_name(log_state),
                       MASTER_CAN_ENABLED_JSON,
                       task_load_percent,
                       task_load_percent,
                       task_load_percent,
                       load_percent,
                       rx_depth,
                       sample_depth,
                       heartbeat_depth,
                       rx_sample_queue_drops,
                       rx_heartbeat_queue_drops,
                       last_write_ms,
                       max_write_ms,
                       sd_write_failures,
                       RX_QUEUE_LENGTH,
                       RX_QUEUE_LENGTH,
                       RX_QUEUE_LENGTH,
                       TRANSM_RATE,
                       can_bus_load_x100,
                       can_bits_per_sec,
                       can_total_fps_x100,
                       can_rx_fps_x100,
                       can_tx_fps_x100,
                       can_rx_frames,
                       can_tx_frames,
                       free_heap_kb,
                       writeCounter,
                       now_us);

    if (len < 0 || len >= (int)out_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t master_format_status_json(char *out, size_t out_size)
{
    size_t used = 0;
    if (append_json(out,
                    out_size,
                    &used,
                    "{\"ok\":true,\"config\":{\"source\":\"%s\",\"path\":\"%s\",\"node_count\":%u,"
                    "\"can_enabled\":%s},"
                    "\"logger\":{\"state\":\"%s\",\"file\":\"%s\",\"blocks\":%d,"
                    "\"log_mode\":\"%s\"},\"master_sensors\":[",
                    node_config_loaded_from_sd ? "sd" : "fallback",
                    node_config_loaded_from_sd ? NODES_CONFIG_PATH : "built-in",
                    (unsigned)configured_node_count,
                    MASTER_CAN_ENABLED_JSON,
                    log_state_name(log_state),
                    current_log_path[0] ? current_log_path : "",
                    writeCounter,
                    master_log_mode_name(runtime_log_mode)) != 0) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t s = 0; s < configured_master_sensor_count; s++) {
        const sensor_config_t *sensor = &configured_master_sensors[s];
        if (append_json(out,
                        out_size,
                        &used,
                        "%s{\"sensor_id\":%u,\"name\":\"%s\",\"can_id\":%lu,"
                        "\"can_id_hex\":\"0x%03" PRIX32 "\",\"units\":\"%s\","
                        "\"sample_rate_hz\":%u,\"function\":\"%s\","
                        "\"port\":%u,\"aux_port\":%u,\"preview_enabled\":%s}",
                        s == 0 ? "" : ",",
                        sensor->sensor_id,
                        sensor->name,
                        (unsigned long)sensor->can_id,
                        sensor->can_id,
                        sensor->units,
                        sensor->sample_rate_hz,
                        sensor->function[0] ? sensor->function : "gps_speed",
                        sensor->port,
                        sensor->aux_port,
                        master_sensor_preview_enabled(sensor) ? "true" : "false") != 0) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (append_json(out, out_size, &used, "],\"nodes\":[") != 0) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < configured_node_count; i++) {
        const node_config_entry_t *node = &configured_nodes[i];
        if (append_json(out,
                        out_size,
                        &used,
                        "%s{\"node_id\":%u,\"name\":\"%s\",\"state_can_id\":%lu,"
                        "\"state_can_id_hex\":\"0x%03" PRIX32 "\",\"active\":%s,"
                        "\"low_power_ack_seen\":%s,\"last_seen_rx_us\":%lld,"
                        "\"last_state_rx_us\":%lld,\"sensors\":[",
                        i == 0 ? "" : ",",
                        node->node_id,
                        node->name,
                        (unsigned long)node->state_can_id,
                        node->state_can_id,
                        node->active ? "true" : "false",
                        node->low_power_ack_seen ? "true" : "false",
                        (long long)node->last_seen_rx_us,
                        (long long)node->last_state_rx_us) != 0) {
            return ESP_ERR_NO_MEM;
        }

        for (size_t s = 0; s < node->sensor_count; s++) {
            const sensor_config_t *sensor = &node->sensors[s];
            if (append_json(out,
                            out_size,
	                        &used,
	                        "%s{\"sensor_id\":%u,\"name\":\"%s\",\"can_id\":%lu,"
	                        "\"can_id_hex\":\"0x%03" PRIX32 "\",\"units\":\"%s\","
	                        "\"sample_rate_hz\":%u,\"function\":\"%s\","
	                        "\"port\":%u,\"aux_port\":%u}",
	                        s == 0 ? "" : ",",
	                        sensor->sensor_id,
	                        sensor->name,
	                        (unsigned long)sensor->can_id,
	                        sensor->can_id,
	                        sensor->units,
	                        sensor->sample_rate_hz,
	                        sensor->function[0] ? sensor->function : "sim",
	                        sensor->port,
	                        sensor->aux_port) != 0) {
                return ESP_ERR_NO_MEM;
            }
        }

        if (append_json(out, out_size, &used, "]}") != 0) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (append_json(out, out_size, &used, "]}") != 0) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* --------------------- SD file download helpers ------------------ */

static bool is_safe_sd_filename(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        return false;
    }

    for (const char *p = filename; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }

    return true;
}

esp_err_t master_open_sd_file_for_download(const char *filename, FILE **out_file, long long *out_size)
{
    if (!out_file || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_file = NULL;
    *out_size = 0;

    if (log_state != LOG_STATE_IDLE) {
        ESP_LOGW(TAG, "Download ignored; stop logging first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_safe_sd_filename(filename)) {
        ESP_LOGW(TAG, "Invalid filename for download: %s", filename ? filename : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    char path[LOG_PATH_MAX_LEN + 32];
    int needed = snprintf(path, sizeof(path), "/sdcard/%s", filename);
    if (needed < 0 || needed >= (int)sizeof(path)) {
        ESP_LOGW(TAG, "Filename too long: %s", filename);
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for download: %s", path);
        return ESP_FAIL;
    }

    *out_file = file;
    *out_size = (long long)st.st_size;
    return ESP_OK;
}

static void download_sd_file(const char *filename)
{
    FILE *file = NULL;
    long long file_size = 0;
    if (master_open_sd_file_for_download(filename, &file, &file_size) != ESP_OK) {
        return;
    }

    uint8_t raw[DOWNLOAD_READ_CHUNK_BYTES];
    uint8_t b64[DOWNLOAD_B64_CHUNK_BYTES];
    size_t total_sent = 0;
    esp_log_level_t previous_log_level = esp_log_level_get("*");

    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("BEGIN_FILE name=%s size=%lld encoding=base64\n", filename, file_size);

    while (1) {
        size_t n = fread(raw, 1, sizeof(raw), file);
        if (n > 0) {
            size_t olen = 0;
            int err = mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, n);
            if (err != 0) {
                printf("ERROR_FILE name=%s reason=base64_encode_failed code=%d\n", filename, err);
                break;
            }

            fwrite(b64, 1, olen, stdout);
            putchar('\n');
            fflush(stdout);
            total_sent += n;
        }

        if (n < sizeof(raw)) {
            if (ferror(file)) {
                printf("ERROR_FILE name=%s reason=read_failed\n", filename);
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(DOWNLOAD_SERIAL_LINE_DELAY_MS));
    }

    fclose(file);

    printf("END_FILE name=%s bytes=%u\n", filename, (unsigned)total_sent);
    fflush(stdout);
    esp_log_level_set("*", previous_log_level);
}

/* --------------------- Runtime tasks ------------------ */

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

    if (!master_can_ready()) {
        ESP_LOGW(TAG, "CAN recovery task exiting; CAN is disabled/not ready");
        vTaskDelete(NULL);
        return;
    }

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
            err = twai_node_recover(node_hdl);
            if (err == ESP_OK) {
                recovery_started = true;
                ESP_LOGW(TAG, "TWAI bus-off recovery started");
            } else {
                ESP_LOGE(TAG, "TWAI bus-off recovery failed to start: %s", esp_err_to_name(err));
            }
        } else if (recovery_started && status.state == TWAI_ERROR_ACTIVE) {
            recovery_started = false;
            can_recovered_since_bus_off = true;
            ESP_LOGI(TAG, "TWAI bus recovered and is error-active");
        }

        vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_POLL_MS));
    }
}

static void node_watchdog_task(void *arg)
{
    (void)arg;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        bool force_reconfigure = can_recovered_since_bus_off;
        can_recovered_since_bus_off = false;

        for (size_t i = 0; i < configured_node_count; i++) {
            node_config_entry_t *node = &configured_nodes[i];
            int64_t silence_us = node->last_seen_rx_us > 0
                               ? now_us - node->last_seen_rx_us
                               : INT64_MAX;

            if (force_reconfigure) {
                node->active = false;
                node->last_seen_rx_us = 0;
                node->last_recovery_attempt_us = 0;
                silence_us = INT64_MAX;
            } else if (silence_us >= (int64_t)NODE_OFFLINE_TIMEOUT_MS * 1000) {
                if (node->active) {
                    ESP_LOGW(TAG,
                             "Node %u (%s) offline after %lld ms without traffic",
                             node->node_id,
                             node->name,
                             (long long)(silence_us / 1000));
                }
                node->active = false;
            }

            bool needs_recovery = !node->active ||
                                  silence_us >= (int64_t)NODE_OFFLINE_TIMEOUT_MS * 1000;
            if (log_state != LOG_STATE_RUNNING ||
                !needs_recovery ||
                (now_us - node->last_recovery_attempt_us) <
                    (int64_t)NODE_RETRY_INTERVAL_MS * 1000) {
                continue;
            }

            node->last_recovery_attempt_us = now_us;
            ESP_LOGW(TAG, "Reconfiguring offline node %u (%s)", node->node_id, node->name);
            send_runtime_config_and_start_node(node);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * rx_dispatch_task is the central fan-out point for received CAN frames:
 * - telemetry_submit_can_frame(): live MQTT preview and health forwarding
 * - handle_node_state_frame(): active/low-power state tracking
 * - twai_rx_sample_queue: binary SD log writer, only while logging
 */
static void rx_dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        can_rx_word_t msg;
        if (xQueueReceive(twai_rx_queue, &msg, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        int64_t dispatch_start_us = esp_timer_get_time();
        
        // Add to heartbeat Queue
        if (msg.id == ID_NODE1_HEARTBEAT) {
            if (xQueueSend(twai_rx_heartbeat_queue, &msg, 1) != pdPASS) {
                rx_heartbeat_queue_drops++;
                ESP_LOGW(TAG, "Heartbeat queue FULL - item dropped");
            }
        }

        mark_node_seen_by_frame(&msg);
        telemetry_submit_can_frame(msg.id, msg.dlc, msg.data);

        if (msg.id >= ID_NODE_STATE_BASE &&
            msg.id < (ID_NODE_STATE_BASE + 0x40)) {
            handle_node_state_frame(&msg);
        } else if (msg.id >= ID_NODE_HEALTH_BASE &&
                   msg.id < (ID_NODE_HEALTH_BASE + 0x40)) {
            handle_node_health_frame(&msg);
        }

        if (log_state == LOG_STATE_RUNNING) {
            if (xQueueSend(twai_rx_sample_queue, &msg, 1) != pdPASS) {
                rx_sample_queue_drops++;
                ESP_LOGW(TAG, "Sample queue FULL - item dropped");
            }
        }

        record_interval_busy_us(&dispatch_busy_us_interval,
                                (uint32_t)(esp_timer_get_time() - dispatch_start_us));

    }
}

/*
 * SD binary log record format:
 *   int64_t[0] = CAN ID
 *   int64_t[1] = packed CAN payload
 *
 * Sensor nodes pack payload as:
 *   low  32 bits = signed/unsigned sensor value
 *   high 32 bits = timestamp_ms
 *
 * The Streamlit dashboard decodes this format to CSV after downloading.
 */
static void write_log_block(const int64_t (*buf)[INFO_PER_SAMPLE], size_t sample_count)
{
    const size_t bytes = sample_count * INFO_PER_SAMPLE * sizeof(int64_t);
    int64_t write_start_us = esp_timer_get_time();

    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        sd_write_failures++;
        return;
    }

    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Log file is not open");
        log_state = LOG_STATE_STOPPING;
        sd_write_failures++;
        return;
    }

    size_t written = fwrite(buf, 1, bytes, logFile);
    fflush(logFile);
    xSemaphoreGive(log_file_mutex);

    uint32_t write_ms = (uint32_t)((esp_timer_get_time() - write_start_us) / 1000);
    sd_write_last_ms = write_ms;
    if (write_ms > sd_write_max_ms) {
        sd_write_max_ms = write_ms;
    }

    if (written != bytes) {
        ESP_LOGE(TAG, "Short write! wrote=%u expected=%u",
                 (unsigned)written, (unsigned)bytes);
        sd_write_failures++;
        return;
    }

    writeCounter++;
    ESP_LOGI(TAG, "Wrote block %d (%u bytes, %u samples)",
             writeCounter, (unsigned)bytes, (unsigned)sample_count);
}

static void sd_writer_task(void *arg)
{
    (void)arg;
    size_t sample_index = 0;
    int64_t log_buffer[SAMPLES_PER_BLOCK][INFO_PER_SAMPLE];

    while (1) { 
        can_rx_word_t msg;
        BaseType_t got_sample = xQueueReceive(twai_rx_sample_queue, &msg, pdMS_TO_TICKS(100));

        if (got_sample == pdTRUE && log_state != LOG_STATE_IDLE) {
            int64_t sd_writer_start_us = esp_timer_get_time();
            uint32_t value32 = (uint32_t)(msg.data & 0xFFFFFFFFu);
            uint32_t ts32    = (uint32_t)((msg.data >> 32) & 0xFFFFFFFFu);

            if (master_log_samples_enabled()) {
                ESP_LOGI(TAG,
                         "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIu32 " | Value: %" PRIu32,
                         msg.id,
                         ts32,
                         value32);
            }

            log_buffer[sample_index][0] = (int64_t)msg.id;
            log_buffer[sample_index][1] = (int64_t)msg.data;
            sample_index++;

            if (sample_index >= SAMPLES_PER_BLOCK) {
                write_log_block(log_buffer, sample_index);
                sample_index = 0;
            }

            record_interval_busy_us(&sd_writer_busy_us_interval,
                                    (uint32_t)(esp_timer_get_time() - sd_writer_start_us));
        }

        if (log_state == LOG_STATE_STOPPING && uxQueueMessagesWaiting(twai_rx_sample_queue) == 0) {
            if (sample_index > 0) {
                ESP_LOGI(TAG, "Flushing partial block (%u samples)...", (unsigned)sample_index);
                write_log_block(log_buffer, sample_index);
                sample_index = 0;
            }

            if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) == pdTRUE) {
                close_log_file_if_open();
                xSemaphoreGive(log_file_mutex);
            }

            log_state = LOG_STATE_IDLE;
            ESP_LOGI(TAG, "Logging stopped.");
        }
    }
}

static void save_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(LOG_SAVE_INTERVAL_MS));

        if (log_state != LOG_STATE_RUNNING) {
            continue;
        }

        if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to lock log file mutex");
            continue;
        }

        if (logFile) {
            ESP_LOGI(TAG, "File saved");
            close_log_file_if_open();
            if (!reopen_log_file_append(current_log_path)) {
                log_state = LOG_STATE_STOPPING;
            }
        }

        xSemaphoreGive(log_file_mutex);
    }
}

/*
Serial command interface:
  start
    Open a fresh SD log file, begin logging CAN frames, and broadcast START to nodes.
  stop
    Broadcast STOP to nodes, stop accepting new log samples, drain queued samples,
    flush the partial block, and close the active log file.
  status
    Print logger state, configured node/sensor state, SD files, and SD capacity.
  files
    Print the SD file list and SD capacity without printing logger/node status.
  download <filename>
    Stream an idle log file from /sdcard as Base64 between BEGIN_FILE/END_FILE markers.
  mqtt <mqtt://host:port>
    Save /sdcard/mqtt_config.json. Reboot after this command for telemetry to
    reconnect to the new broker.
  log <off|master|status|node|samples|all>
    Runtime debug printing. Default is off. master prints master RX samples;
    status prints low-rate node status/beacon logs; node prints node TX samples;
    samples prints master RX + node TX samples; all enables everything.
  help or ?
    Print the short command list.
*/
static void serial_command_task(void *arg)
{
    (void)arg;
    char line[128];
    char original_line[128];

    ESP_LOGI(TAG, "Serial commands ready: start, stop, status");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        snprintf(original_line, sizeof(original_line), "%s", line);

        char *cmd = line;
        while (isspace((unsigned char)*cmd)) {
            cmd++;
        }

        char *end = cmd + strlen(cmd);
        while (end > cmd && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }

        for (char *p = cmd; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }

        if (strcmp(cmd, "start") == 0) {
            (void)master_start_logging();
        } else if (strcmp(cmd, "stop") == 0) {
            (void)master_stop_logging();
        } else if (strcmp(cmd, "files") == 0) {
            print_sd_card_status();
        } else if (strncmp(cmd, "download ", 9) == 0) {
            char *filename = cmd + 9;
            while (isspace((unsigned char)*filename)) {
                filename++;
            }
            download_sd_file(filename);
        } else if (strncmp(cmd, "mqtt ", 5) == 0) {
            char *uri = original_line;
            while (isspace((unsigned char)*uri)) {
                uri++;
            }
            while (*uri && !isspace((unsigned char)*uri)) {
                uri++;
            }
            while (isspace((unsigned char)*uri)) {
                uri++;
            }
            char *uri_end = uri + strlen(uri);
            while (uri_end > uri && isspace((unsigned char)uri_end[-1])) {
                *--uri_end = '\0';
            }

            esp_err_t err = save_mqtt_broker_uri(uri);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "MQTT broker set to %s", uri);
                ESP_LOGI(TAG, "Reboot the master for the new MQTT broker to take effect.");
            } else {
                ESP_LOGW(TAG,
                         "Failed to save MQTT broker URI '%s': %s. Use mqtt://host:1883",
                         uri,
                         esp_err_to_name(err));
            }
        } else if (strncmp(cmd, "log", 3) == 0 &&
                   (cmd[3] == '\0' || isspace((unsigned char)cmd[3]))) {
            char *mode = cmd + 3;
            while (isspace((unsigned char)*mode)) {
                mode++;
            }
            if (mode[0] == '\0') {
                ESP_LOGI(TAG, "Log mode: %s", master_log_mode_name(runtime_log_mode));
            } else {
                (void)master_set_log_mode_text(mode);
            }
        } else if (strcmp(cmd, "status") == 0) {
            ESP_LOGI(TAG, "Config source: %s (%s), nodes=%u",
                     node_config_loaded_from_sd ? "sd" : "fallback",
                     node_config_loaded_from_sd ? NODES_CONFIG_PATH : "built-in",
                     (unsigned)configured_node_count);
            ESP_LOGI(TAG, "Logger status: %s, file=%s, blocks=%d",
                     log_state_name(log_state),
                     current_log_path[0] ? current_log_path : "(none)",
                     writeCounter);
            ESP_LOGI(TAG, "Log mode: %s", master_log_mode_name(runtime_log_mode));
            print_node_power_summary();
            print_sd_card_status();
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            ESP_LOGI(TAG,
                     "Commands: start, stop, status, files, download <filename>, "
                     "mqtt <mqtt://host:port>, log <off|master|status|node|samples|all>");
        } else if (cmd[0] != '\0') {
            ESP_LOGW(TAG, "Unknown command: %s", cmd);
        }
    }
}

static void time_beacon_task(void *arg)
{
    (void)arg;
    uint8_t raw[8];
    uint32_t beacon_count = 0;
 
    twai_frame_t beacon = {
        .header.id = ID_MASTER_TIME_BEACON,
        .header.ide = false,
        .buffer = raw,
        .buffer_len = 8,
    };

    if (!master_can_ready()) {
        ESP_LOGW(TAG, "Time beacon task exiting; CAN is disabled/not ready");
        vTaskDelete(NULL);
        return;
    }
 
    while (1) {
        uint64_t t_main_us = (uint64_t)esp_timer_get_time();
 
        for (int i = 0; i < 8; i++) {
            raw[i] = (uint8_t)((t_main_us >> (8 * i)) & 0xFF);
        }

 
        esp_err_t err = twai_node_transmit(node_hdl, &beacon, 0);
        beacon_count++;
        if (err == ESP_OK) {
            record_can_tx_frame(8);
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Time beacon TX failed: %s", esp_err_to_name(err));
        } else if ((beacon_count % 50) == 0) {
            ESP_LOGD(TAG, "Time beacon TX | ts=%" PRIu64, t_main_us);
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
 
    vTaskDelete(NULL);
}

static void auto_start_logging_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(MASTER_AUTO_START_DELAY_MS));

    if (master_start_logging()) {
        ESP_LOGI(TAG, "Auto-start logging enabled; recording started after boot");
    } else {
        ESP_LOGW(TAG, "Auto-start logging enabled but start failed");
    }

    vTaskDelete(NULL);
}

/* Tasks to add:
Heartbeat
Error Handling (reset CAN bus)
dispatcher (duplicate info into multi queues? Wifi queue, SD queue, Motor Queue)
*/



/* --------------------- app_main ------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // Power SD IO via on-chip LDO channel 4
    sd_pwr_ctrl_ldo_config_t cfg = {};
    cfg.ldo_chan_id = 4; // CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID=4

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

    slot_config.clk = (gpio_num_t)43;
    slot_config.cmd = (gpio_num_t)44;
    slot_config.d0  = (gpio_num_t)39;
    slot_config.d1  = (gpio_num_t)40;
    slot_config.d2  = (gpio_num_t)41;
    slot_config.d3  = (gpio_num_t)42;

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Avoid “phantom” CD/WP defaults
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;

    //give the card a moment after boot (often helps OCR timeouts)
    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_card_print_info(stdout, card);

#if MASTER_USE_SD_NODE_CONFIG
    if (load_node_config_from_sd() != ESP_OK) {
        load_default_node_config();
        if (create_node_config_file_from_current() == ESP_OK) {
            (void)load_node_config_from_sd();
        }
    }
#else
    load_default_node_config();
    ESP_LOGW(TAG,
             "SD node config disabled; using hardcoded firmware node/sensor config");
#endif

    // ---------------- QUEU/SEMAPHORE INIT ------------------
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI RX queue");
        return;
    }

    twai_rx_heartbeat_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_heartbeat_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI heartbeat queue");
        return;
    }

    twai_rx_sample_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_sample_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI sample queue");
        return;
    }

    log_file_mutex = xSemaphoreCreateMutex();
    if (!log_file_mutex) {
        ESP_LOGE(TAG, "Failed to create log_file_mutex");
        return;
    }


    // -------------------- START CAN -----------------------------
#if MASTER_CAN_ENABLED
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled");
    ESP_LOGI(TAG, "Sending startup STOP command to force nodes idle");
    for (int i = 0; i < 3; i++) {
        (void)send_control_command(ID_STOP_CMD);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#else
    ESP_LOGW(TAG, "CAN disabled for this build; skipping TWAI startup");
#endif

    xTaskCreate(sd_writer_task, "sd_writer", 8192, NULL, 10, &sd_task_handle);
    xTaskCreate(save_task,      "save_task", 4096, NULL, 10, &save_task_handle);
    xTaskCreate(serial_command_task, "serial_cmd", 4096, NULL, 9, &serial_task_handle);
#if MASTER_CAN_ENABLED
    xTaskCreate(rx_dispatch_task, "dispatch",    4096, NULL,        8,  &dispatch_task_handle);
    xTaskCreatePinnedToCore(time_beacon_task, "time_beacon", 4096, NULL, 7, NULL, tskNO_AFFINITY);
    xTaskCreate(can_recovery_task, "can_recovery", 4096, NULL, 9, &can_recovery_task_handle);
    xTaskCreate(node_watchdog_task, "node_watchdog", 4096, NULL, 6, &node_watchdog_task_handle);
#endif

    esp_err_t gps_err = gps_start();
    if (gps_err != ESP_OK) {
        ESP_LOGW(TAG, "GPS unavailable: %s", esp_err_to_name(gps_err));
    }

    esp_err_t rpm_err = rpm_sampler_start();
    if (rpm_err != ESP_OK) {
        ESP_LOGW(TAG, "RPM sampler unavailable: %s", esp_err_to_name(rpm_err));
    }

#if MASTER_AUTO_START_LOGGING
    xTaskCreate(auto_start_logging_task, "auto_start_log", 4096, NULL, 5, NULL);
#endif

#if MASTER_WIFI_CONTROL_ENABLED
    esp_err_t wifi_err = wifi_control_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi control unavailable: %s", esp_err_to_name(wifi_err));
    } else {
        esp_err_t telemetry_err = telemetry_start();
        if (telemetry_err != ESP_OK) {
            ESP_LOGW(TAG, "Telemetry unavailable: %s", esp_err_to_name(telemetry_err));
        }
    }
#else
    ESP_LOGW(TAG, "Wi-Fi control disabled for autonomous logging build");
#endif

    ESP_LOGI(TAG,
             "Data logger ready. Auto-start logging=%s. Serial commands remain available.",
             MASTER_AUTO_START_LOGGING ? "on" : "off");
}
