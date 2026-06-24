#include "telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "mqtt_client.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "master_control.h"
#include "wifi_control.h"

static const char *TAG = "telemetry";

/*
 * MQTT telemetry bridge map
 * -------------------------
 * This file is the internet-facing side of the master. It never owns the SD
 * logger directly; it calls functions from master_control.h for start/stop,
 * status JSON, file opening, and config changes.
 *
 * Topic roles:
 * - /status   slow logger/node summary for dashboard cards
 * - /can      live CAN preview, with optional high-resolution selected IDs
 * - /health   master and node health/load summaries
 * - /files    SD card file list
 * - /download resumable base64 file transfer windows
 * - /config   nodes_config.json read/save/reload responses
 * - /command  commands from dashboard/computer to the master
 *
 * Field notes:
 * - SD card remains the source of truth. MQTT can drop and recover.
 * - File downloads are chunk-window based; the dashboard asks for missing
 *   windows until the file is complete.
 * - Live CAN and health telemetry are paused while a download window is being
 *   sent to reduce broker/hotspot pressure.
 */

/* --------------------- MQTT topics and limits ------------------ */

#define TELEMETRY_MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define TELEMETRY_MQTT_CONFIG_PATH "/sdcard/mqtt_config.json"
#define TELEMETRY_MQTT_CONFIG_MAX_BYTES 512
#define TELEMETRY_MQTT_URI_MAX 128
#define TELEMETRY_MQTT_CLIENT_ID_PREFIX "baja_logger_master"
#define TELEMETRY_STATUS_TOPIC    "baja/logger/master/status"
#define TELEMETRY_CAN_TOPIC       "baja/logger/master/can"
#define TELEMETRY_GPS_TOPIC       "baja/logger/master/gps"
#define TELEMETRY_COMMAND_TOPIC   "baja/logger/master/command"
#define TELEMETRY_FILES_TOPIC     "baja/logger/master/files"
#define TELEMETRY_DOWNLOAD_TOPIC  "baja/logger/master/download"
#define TELEMETRY_CONFIG_TOPIC    "baja/logger/master/config"
#define TELEMETRY_HEALTH_TOPIC    "baja/logger/master/health"
#define TELEMETRY_COMMAND_TOKEN   "baja_logger_test_v1"
#define TELEMETRY_MQTT_NETWORK_TIMEOUT_MS 10000
#define TELEMETRY_MQTT_RECONNECT_TIMEOUT_MS 5000
#define TELEMETRY_MQTT_RECOVERY_ERROR_LIMIT 3
#define TELEMETRY_MQTT_BUFFER_BYTES 32768
#define TELEMETRY_PERIOD_MS       5000
#define TELEMETRY_CAN_PERIOD_MS    100
#define TELEMETRY_HEALTH_PERIOD_MS 1000
#define TELEMETRY_JSON_MAX        8192
#define TELEMETRY_FILES_JSON_MAX 16384
#define TELEMETRY_CAN_JSON_MAX     192
#define TELEMETRY_HEALTH_JSON_MAX  768
#define TELEMETRY_CAN_QUEUE_LENGTH  64
#define TELEMETRY_CAN_HIGH_RES_QUEUE_LENGTH 1024
#define TELEMETRY_HEALTH_QUEUE_LENGTH 16
#define TELEMETRY_GPS_QUEUE_LENGTH 1
#define TELEMETRY_CAN_LATEST_SLOTS  16
#define TELEMETRY_CAN_HIGH_RES_MAX_IDS 8
#define TELEMETRY_CAN_HIGH_RES_INTERVAL_US 10000
#define TELEMETRY_CAN_HIGH_RES_BATCH_FRAMES 24
#define TELEMETRY_CAN_HIGH_RES_MAX_PER_PERIOD 240
#define TELEMETRY_COMMAND_QUEUE_LENGTH 4
#define TELEMETRY_COMMAND_PAYLOAD_MAX 24576
#define TELEMETRY_COMMAND_NAME_MAX 32
#define TELEMETRY_DOWNLOAD_FILENAME_MAX 64
#define TELEMETRY_DOWNLOAD_RAW_BYTES 192
#define TELEMETRY_DOWNLOAD_B64_BYTES (((TELEMETRY_DOWNLOAD_RAW_BYTES + 2) / 3) * 4)
#define TELEMETRY_DOWNLOAD_CHUNK_DELAY_MS 150
#define TELEMETRY_DOWNLOAD_DEFAULT_WINDOW_CHUNKS 8
#define TELEMETRY_DOWNLOAD_MAX_WINDOW_CHUNKS 96
#define TELEMETRY_HTTP_UPLOAD_BASE_URL "http://138.197.132.56"
#define TELEMETRY_HTTP_UPLOAD_TOKEN TELEMETRY_COMMAND_TOKEN
#define TELEMETRY_HTTP_UPLOAD_CHUNK_BYTES 2048
#define TELEMETRY_HTTP_RESPONSE_MAX 1024
#define TELEMETRY_HTTP_URL_MAX 256
#define TELEMETRY_HTTP_REQUEST_TIMEOUT_MS 30000
#define TELEMETRY_HTTP_UPLOAD_RETRIES 5
#define TELEMETRY_HTTP_UPLOAD_RETRY_DELAY_MS 1500
#define TELEMETRY_HTTP_PROGRESS_STEP_BYTES (256 * 1024)
#define TELEMETRY_HTTP_UPLOAD_STREAM_BUFFER_BYTES (32 * 1024)
#define TELEMETRY_HTTP_UPLOAD_TX_BUFFER_BYTES (16 * 1024)
#define TELEMETRY_HTTP_UPLOAD_YIELD_STEP_BYTES (512 * 1024)
#define TELEMETRY_TASK_STACK_BYTES 6144
#define TELEMETRY_COMMAND_TASK_STACK_BYTES 12288
#define ID_NODE_HEALTH_BASE      0x180
#define NODE_SAMPLE_QUEUE_CAPACITY 64

#define MQTT_CONNECTED_BIT BIT0

typedef enum {
    TELEMETRY_COMMAND_START = 0,
    TELEMETRY_COMMAND_STOP,
    TELEMETRY_COMMAND_FILES,
    TELEMETRY_COMMAND_DOWNLOAD,
    TELEMETRY_COMMAND_CONFIG_GET,
    TELEMETRY_COMMAND_CONFIG_SAVE,
    TELEMETRY_COMMAND_CONFIG_RELOAD,
    TELEMETRY_COMMAND_LOG,
    TELEMETRY_COMMAND_PREVIEW_CONFIG,
} telemetry_command_type_t;

typedef struct {
    telemetry_command_type_t type;
    char filename[TELEMETRY_DOWNLOAD_FILENAME_MAX];
    char *config_text;
    size_t config_text_len;
    char log_mode[16];
    uint32_t request_id;
    uint32_t start_seq;
    uint32_t max_chunks;
    bool apply_config;
    bool preview_all_samples;
    uint32_t preview_can_ids[TELEMETRY_CAN_HIGH_RES_MAX_IDS];
    size_t preview_can_id_count;
} telemetry_command_t;

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint64_t data;
} telemetry_can_frame_t;

static EventGroupHandle_t s_mqtt_event_group;
static QueueHandle_t s_can_queue;
static QueueHandle_t s_can_high_res_queue;
static QueueHandle_t s_health_queue;
static QueueHandle_t s_gps_queue;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_publish_mutex;
static esp_mqtt_client_handle_t s_mqtt_client;
static TaskHandle_t s_telemetry_task_handle;
static TaskHandle_t s_command_task_handle;
static char s_mqtt_client_id[48];
static char s_mqtt_broker_uri[TELEMETRY_MQTT_URI_MAX];
static uint32_t s_mqtt_transport_error_count;
static volatile bool s_download_active;
static uint32_t s_download_guard_request_id;
static uint32_t s_download_guard_high_water_seq;
static char s_download_guard_filename[TELEMETRY_DOWNLOAD_FILENAME_MAX];
static portMUX_TYPE s_preview_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_preview_can_ids[TELEMETRY_CAN_HIGH_RES_MAX_IDS];
static int64_t s_preview_last_queue_time_us[TELEMETRY_CAN_HIGH_RES_MAX_IDS];
static size_t s_preview_can_id_count;
static bool s_preview_all_samples;

/* --------------------- Broker config and MQTT utilities ------------------ */

static void load_mqtt_broker_uri(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", TELEMETRY_MQTT_BROKER_URI);

    FILE *file = fopen(TELEMETRY_MQTT_CONFIG_PATH, "rb");
    if (!file) {
        ESP_LOGW(TAG,
                 "Missing %s; using default broker %s",
                 TELEMETRY_MQTT_CONFIG_PATH,
                 out);
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        ESP_LOGW(TAG, "Failed to read %s; using default broker", TELEMETRY_MQTT_CONFIG_PATH);
        return;
    }

    long file_size = ftell(file);
    if (file_size <= 0 || file_size > TELEMETRY_MQTT_CONFIG_MAX_BYTES) {
        fclose(file);
        ESP_LOGW(TAG, "Invalid %s size; using default broker", TELEMETRY_MQTT_CONFIG_PATH);
        return;
    }

    rewind(file);
    char *text = calloc(1, (size_t)file_size + 1);
    if (!text) {
        fclose(file);
        ESP_LOGW(TAG, "No memory for MQTT config; using default broker");
        return;
    }

    size_t read_len = fread(text, 1, (size_t)file_size, file);
    fclose(file);
    if (read_len != (size_t)file_size) {
        free(text);
        ESP_LOGW(TAG, "Short read from %s; using default broker", TELEMETRY_MQTT_CONFIG_PATH);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse %s; using default broker", TELEMETRY_MQTT_CONFIG_PATH);
        return;
    }

    const cJSON *broker_uri = cJSON_GetObjectItemCaseSensitive(root, "broker_uri");
    if (cJSON_IsString(broker_uri) &&
        broker_uri->valuestring &&
        broker_uri->valuestring[0] != '\0') {
        snprintf(out, out_size, "%s", broker_uri->valuestring);
        ESP_LOGI(TAG, "Loaded MQTT broker from %s: %s", TELEMETRY_MQTT_CONFIG_PATH, out);
    } else {
        ESP_LOGW(TAG, "%s missing string broker_uri; using default broker", TELEMETRY_MQTT_CONFIG_PATH);
    }

    cJSON_Delete(root);
}

static bool mqtt_event_topic_equals(const esp_mqtt_event_handle_t event, const char *topic)
{
    size_t expected_len = strlen(topic);
    return event &&
           event->topic &&
           event->topic_len == expected_len &&
           memcmp(event->topic, topic, expected_len) == 0;
}

static void lowercase_in_place(char *text)
{
    for (char *p = text; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static void strip_whitespace(char *text)
{
    char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static bool mqtt_is_connected(void)
{
    return s_mqtt_event_group &&
           (xEventGroupGetBits(s_mqtt_event_group) & MQTT_CONNECTED_BIT);
}

static int mqtt_publish_locked(const char *topic, const char *payload, int qos)
{
    if (!s_mqtt_client || !s_publish_mutex) {
        return -1;
    }

    if (!mqtt_is_connected()) {
        return -1;
    }

    if (xSemaphoreTake(s_publish_mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    if (!mqtt_is_connected()) {
        xSemaphoreGive(s_publish_mutex);
        return -1;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                         topic,
                                         payload,
                                         0,
                                         qos,
                                         0);
    xSemaphoreGive(s_publish_mutex);
    return msg_id;
}

static int mqtt_publish_json_locked(const char *topic, cJSON *root, int qos)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return -1;
    }

    int msg_id = mqtt_publish_locked(topic, payload, qos);
    free(payload);
    return msg_id;
}

static bool json_append(char *out, size_t out_size, size_t *pos, const char *fmt, ...)
{
    if (!out || !pos || *pos >= out_size) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *pos, out_size - *pos, fmt, args);
    va_end(args);

    if (written < 0 || written >= (int)(out_size - *pos)) {
        return false;
    }

    *pos += (size_t)written;
    return true;
}

static void format_e7_decimal(int32_t value_e7, char *out, size_t out_size)
{
    int64_t value = value_e7;
    const char *sign = "";
    if (value < 0) {
        sign = "-";
        value = -value;
    }

    snprintf(out,
             out_size,
             "%s%" PRId64 ".%07" PRId64,
             sign,
             value / 10000000,
             value % 10000000);
}

/* --------------------- Command parsing and queueing ------------------ */

static void free_command_resources(telemetry_command_t *command)
{
    if (!command) {
        return;
    }

    free(command->config_text);
    command->config_text = NULL;
    command->config_text_len = 0;
}

static void queue_command_request(telemetry_command_t *command)
{
    if (s_download_active && command->type == TELEMETRY_COMMAND_DOWNLOAD) {
        ESP_LOGW(TAG,
                 "Ignoring download command while another upload is active: %s request=%" PRIu32,
                 command->filename,
                 command->request_id);
        free_command_resources(command);
        return;
    }

    if (!s_command_queue) {
        ESP_LOGW(TAG, "MQTT command queue not ready");
        free_command_resources(command);
        return;
    }

    if (xQueueSend(s_command_queue, command, 0) == pdPASS) {
        return;
    }

    telemetry_command_t dropped;
    (void)xQueueReceive(s_command_queue, &dropped, 0);
    free_command_resources(&dropped);
    if (xQueueSend(s_command_queue, command, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT command queue full - newest command dropped");
        free_command_resources(command);
    }
}

static bool fill_command_from_text(char *command_text, telemetry_command_t *command)
{
    strip_whitespace(command_text);
    char *argument = command_text;
    while (*argument && !isspace((unsigned char)*argument)) {
        argument++;
    }

    if (*argument) {
        *argument++ = '\0';
        strip_whitespace(argument);
    }

    lowercase_in_place(command_text);

    memset(command, 0, sizeof(*command));
    command->request_id = esp_random();
    command->apply_config = true;
    command->max_chunks = TELEMETRY_DOWNLOAD_DEFAULT_WINDOW_CHUNKS;

    if (strcmp(command_text, "start") == 0) {
        command->type = TELEMETRY_COMMAND_START;
        return true;
    }

    if (strcmp(command_text, "stop") == 0) {
        command->type = TELEMETRY_COMMAND_STOP;
        return true;
    }

    if (strcmp(command_text, "files") == 0) {
        command->type = TELEMETRY_COMMAND_FILES;
        return true;
    }

    if (strcmp(command_text, "download") == 0) {
        command->type = TELEMETRY_COMMAND_DOWNLOAD;
        if (argument[0] != '\0') {
            snprintf(command->filename, sizeof(command->filename), "%s", argument);
        }
        return true;
    }

    if (strcmp(command_text, "config_get") == 0 || strcmp(command_text, "config") == 0) {
        command->type = TELEMETRY_COMMAND_CONFIG_GET;
        return true;
    }

    if (strcmp(command_text, "config_reload") == 0) {
        command->type = TELEMETRY_COMMAND_CONFIG_RELOAD;
        return true;
    }

    if (strcmp(command_text, "config_save") == 0) {
        command->type = TELEMETRY_COMMAND_CONFIG_SAVE;
        return true;
    }

    if (strcmp(command_text, "log") == 0) {
        command->type = TELEMETRY_COMMAND_LOG;
        if (argument[0] != '\0') {
            snprintf(command->log_mode, sizeof(command->log_mode), "%s", argument);
        }
        return true;
    }

    if (strcmp(command_text, "preview_config") == 0) {
        command->type = TELEMETRY_COMMAND_PREVIEW_CONFIG;
        return true;
    }

    return false;
}

static char *copy_json_string_value(const cJSON *item, size_t *out_len)
{
    if (!cJSON_IsString(item) || !item->valuestring) {
        return NULL;
    }

    size_t len = strlen(item->valuestring);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, item->valuestring, len + 1);
    if (out_len) {
        *out_len = len;
    }
    return copy;
}

static void handle_command_payload(const esp_mqtt_event_handle_t event)
{
    if (!event || !event->data || event->data_len <= 0) {
        return;
    }

    if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        ESP_LOGW(TAG,
                 "Ignoring partial MQTT command fragment offset=%d len=%d total=%d",
                 event->current_data_offset,
                 event->data_len,
                 event->total_data_len);
        return;
    }

    if (event->data_len >= TELEMETRY_COMMAND_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "MQTT command payload too large");
        return;
    }

    char *payload = calloc(1, (size_t)event->data_len + 1);
    if (!payload) {
        ESP_LOGE(TAG, "No memory for MQTT command payload");
        return;
    }

    memcpy(payload, event->data, event->data_len);
    payload[event->data_len] = '\0';

    cJSON *root = cJSON_Parse(payload);
    if (root) {
        const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
        if (!cJSON_IsString(token_item) ||
            !token_item->valuestring ||
            strcmp(token_item->valuestring, TELEMETRY_COMMAND_TOKEN) != 0) {
            ESP_LOGW(TAG,
                     "Ignoring MQTT command with missing/invalid token len=%d",
                     event->data_len);
            cJSON_Delete(root);
            free(payload);
            return;
        }

        const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        if (cJSON_IsString(cmd_item) && cmd_item->valuestring) {
            char command_text[TELEMETRY_COMMAND_NAME_MAX] = {};
            snprintf(command_text, sizeof(command_text), "%s", cmd_item->valuestring);
            ESP_LOGI(TAG, "Accepted MQTT command: %s", command_text);

            telemetry_command_t command = {};
            if (!fill_command_from_text(command_text, &command)) {
                ESP_LOGW(TAG, "Unknown MQTT command: %s", command_text);
                cJSON_Delete(root);
                free(payload);
                return;
            }

            const cJSON *request_id_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
            if (cJSON_IsNumber(request_id_item)) {
                command.request_id = (uint32_t)request_id_item->valuedouble;
            }

            if (command.type == TELEMETRY_COMMAND_DOWNLOAD) {
                const cJSON *file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
                if (!cJSON_IsString(file_item) || !file_item->valuestring) {
                    file_item = cJSON_GetObjectItemCaseSensitive(root, "filename");
                }

                if (!cJSON_IsString(file_item) || !file_item->valuestring) {
                    ESP_LOGW(TAG, "MQTT download command missing file");
                    cJSON_Delete(root);
                    free(payload);
                    return;
                }

                snprintf(command.filename, sizeof(command.filename), "%s", file_item->valuestring);

                const cJSON *start_seq_item = cJSON_GetObjectItemCaseSensitive(root, "start_seq");
                if (cJSON_IsNumber(start_seq_item) && start_seq_item->valuedouble > 0) {
                    command.start_seq = (uint32_t)start_seq_item->valuedouble;
                }

                const cJSON *max_chunks_item = cJSON_GetObjectItemCaseSensitive(root, "max_chunks");
                if (cJSON_IsNumber(max_chunks_item) && max_chunks_item->valuedouble > 0) {
                    command.max_chunks = (uint32_t)max_chunks_item->valuedouble;
                    if (command.max_chunks > TELEMETRY_DOWNLOAD_MAX_WINDOW_CHUNKS) {
                        command.max_chunks = TELEMETRY_DOWNLOAD_MAX_WINDOW_CHUNKS;
                    }
                }
            }

            if (command.type == TELEMETRY_COMMAND_LOG) {
                const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
                if (!cJSON_IsString(mode_item) || !mode_item->valuestring) {
                    ESP_LOGW(TAG, "MQTT log command missing mode");
                    cJSON_Delete(root);
                    free(payload);
                    return;
                }

                snprintf(command.log_mode, sizeof(command.log_mode), "%s", mode_item->valuestring);
            }

            if (command.type == TELEMETRY_COMMAND_PREVIEW_CONFIG) {
                const cJSON *all_samples_item = cJSON_GetObjectItemCaseSensitive(root, "all_samples");
                command.preview_all_samples = cJSON_IsTrue(all_samples_item);

                const cJSON *can_ids_item = cJSON_GetObjectItemCaseSensitive(root, "can_ids");
                if (cJSON_IsArray(can_ids_item)) {
                    const cJSON *can_id_item = NULL;
                    cJSON_ArrayForEach(can_id_item, can_ids_item) {
                        if (command.preview_can_id_count >= TELEMETRY_CAN_HIGH_RES_MAX_IDS) {
                            break;
                        }
                        if (!cJSON_IsNumber(can_id_item) ||
                            can_id_item->valuedouble < 0 ||
                            can_id_item->valuedouble > 0x1FFFFFFF) {
                            continue;
                        }
                        command.preview_can_ids[command.preview_can_id_count++] =
                            (uint32_t)can_id_item->valuedouble;
                    }
                }
            }

            if (strcmp(command_text, "config_save") == 0) {
                command.type = TELEMETRY_COMMAND_CONFIG_SAVE;
            }

            if (command.type == TELEMETRY_COMMAND_CONFIG_SAVE) {
                const cJSON *apply_item = cJSON_GetObjectItemCaseSensitive(root, "apply");
                command.apply_config = !cJSON_IsBool(apply_item) || cJSON_IsTrue(apply_item);

                const cJSON *config_text_item = cJSON_GetObjectItemCaseSensitive(root, "config_text");
                command.config_text = copy_json_string_value(config_text_item, &command.config_text_len);

                if (!command.config_text) {
                    const cJSON *config_item = cJSON_GetObjectItemCaseSensitive(root, "config");
                    if (config_item) {
                        command.config_text = cJSON_Print(config_item);
                        command.config_text_len = command.config_text ? strlen(command.config_text) : 0;
                    }
                }

                if (!command.config_text) {
                    ESP_LOGW(TAG, "MQTT config_save command missing config");
                    cJSON_Delete(root);
                    free(payload);
                    return;
                }
            }

            cJSON_Delete(root);
            queue_command_request(&command);
            free(payload);
            return;
        }

        cJSON_Delete(root);
        ESP_LOGW(TAG, "MQTT command JSON missing string field: cmd");
        free(payload);
        return;
    }

    ESP_LOGW(TAG, "Ignoring non-JSON MQTT command without token: %s", payload);
    free(payload);
}

/* --------------------- MQTT event handler ------------------ */

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_transport_error_count = 0;
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        esp_mqtt_client_subscribe(s_mqtt_client, TELEMETRY_COMMAND_TOPIC, 0);
        ESP_LOGI(TAG, "Subscribed to command topic: %s", TELEMETRY_COMMAND_TOPIC);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DATA:
        if (mqtt_event_topic_equals(event, TELEMETRY_COMMAND_TOPIC)) {
            handle_command_payload(event);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        if (event && event->error_handle) {
            ESP_LOGW(TAG,
                     "MQTT error details: type=%d tls_err=0x%x tls_stack=%d sock_errno=%d (%s) connect_code=%d",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno),
                     event->error_handle->connect_return_code);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                s_mqtt_transport_error_count++;
                if (s_mqtt_transport_error_count >= TELEMETRY_MQTT_RECOVERY_ERROR_LIMIT) {
                    s_mqtt_transport_error_count = 0;
                    esp_err_t recover_err = wifi_control_reconnect(
                        wifi_control_is_connected()
                            ? "repeated MQTT transport errors"
                            : "repeated MQTT transport errors while Wi-Fi is offline");
                    if (recover_err != ESP_OK) {
                        ESP_LOGW(TAG,
                                 "Wi-Fi recovery request failed: %s",
                                 esp_err_to_name(recover_err));
                    }
                }
            }
        }
        break;
    default:
        break;
    }
}

/* --------------------- Files, downloads, and config responses ------------------ */

static void publish_files_telemetry(void)
{
    char *json = calloc(1, TELEMETRY_FILES_JSON_MAX);
    if (!json) {
        ESP_LOGE(TAG, "No memory for files JSON buffer");
        return;
    }

    esp_err_t format_err = master_format_files_json(json, TELEMETRY_FILES_JSON_MAX);
    if (format_err == ESP_OK) {
        size_t payload_bytes = strlen(json);
        int msg_id = mqtt_publish_locked(TELEMETRY_FILES_TOPIC, json, 1);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "Failed to publish files telemetry");
        } else {
            ESP_LOGI(TAG,
                     "Published files telemetry msg_id=%d bytes=%u",
                     msg_id,
                     (unsigned)payload_bytes);
        }
    } else {
        ESP_LOGE(TAG,
                 "Failed to format files telemetry: %s buffer=%u",
                 esp_err_to_name(format_err),
                 TELEMETRY_FILES_JSON_MAX);
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "files_json_too_large");
            cJSON_AddStringToObject(root, "code_name", esp_err_to_name(format_err));
            (void)mqtt_publish_json_locked(TELEMETRY_FILES_TOPIC, root, 1);
            cJSON_Delete(root);
        }
    }

    free(json);
}

static void publish_download_error(const char *filename,
                                   uint32_t request_id,
                                   const char *reason,
                                   esp_err_t code)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "event", "error");
    cJSON_AddStringToObject(root, "name", filename ? filename : "");
    cJSON_AddNumberToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "reason", reason);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "code_name", esp_err_to_name(code));
    (void)mqtt_publish_json_locked(TELEMETRY_DOWNLOAD_TOPIC, root, 1);
    cJSON_Delete(root);
}

static bool publish_download_event(cJSON *root)
{
    return mqtt_publish_json_locked(TELEMETRY_DOWNLOAD_TOPIC, root, 1) >= 0;
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} telemetry_http_response_t;

static esp_err_t http_response_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || !event->data) {
        return ESP_OK;
    }

    telemetry_http_response_t *response = (telemetry_http_response_t *)event->user_data;
    size_t remaining = response->capacity - response->length - 1;
    size_t copy_len = (event->data_len < remaining) ? event->data_len : remaining;
    if (copy_len > 0) {
        memcpy(response->buffer + response->length, event->data, copy_len);
        response->length += copy_len;
        response->buffer[response->length] = '\0';
    }

    return ESP_OK;
}

static esp_err_t http_request(const char *url,
                              esp_http_client_method_t method,
                              const void *body,
                              size_t body_len,
                              const char *content_type,
                              char *response,
                              size_t response_size)
{
    telemetry_http_response_t response_state = {
        .buffer = response,
        .capacity = response_size,
        .length = 0,
    };
    if (response && response_size > 0) {
        response[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TELEMETRY_HTTP_REQUEST_TIMEOUT_MS,
        .event_handler = http_response_event_handler,
        .user_data = &response_state,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "X-Device-Token", TELEMETRY_HTTP_UPLOAD_TOKEN);
    if (content_type) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (body && body_len > 0) {
        esp_http_client_set_post_field(client, (const char *)body, body_len);
    }

    ESP_LOGI(TAG, "HTTP %s %s", method == HTTP_METHOD_POST ? "POST" :
                                method == HTTP_METHOD_PUT ? "PUT" : "GET", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s err=%s", url, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %s failed with status=%d response=%s", url, status, response ? response : "");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void publish_upload_progress(const char *filename,
                                    uint32_t request_id,
                                    const char *file_id,
                                    long long uploaded,
                                    long long total)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "event", "upload_progress");
    cJSON_AddStringToObject(root, "name", filename ? filename : "");
    cJSON_AddNumberToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "file_id", file_id ? file_id : "");
    cJSON_AddNumberToObject(root, "uploaded", uploaded);
    cJSON_AddNumberToObject(root, "size", total);
    (void)publish_download_event(root);
    cJSON_Delete(root);
}

static esp_err_t upload_start(const char *filename,
                              uint32_t request_id,
                              long long file_size,
                              char *file_id,
                              size_t file_id_size)
{
    char url[TELEMETRY_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "%s/upload/start", TELEMETRY_HTTP_UPLOAD_BASE_URL);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "filename", filename);
    cJSON_AddNumberToObject(root, "size", file_size);
    cJSON_AddStringToObject(root, "device_id", TELEMETRY_MQTT_CLIENT_ID_PREFIX);
    cJSON_AddNumberToObject(root, "expected_chunks",
                            (file_size + TELEMETRY_HTTP_UPLOAD_CHUNK_BYTES - 1) /
                            TELEMETRY_HTTP_UPLOAD_CHUNK_BYTES);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char response[TELEMETRY_HTTP_RESPONSE_MAX];
    esp_err_t err = http_request(url,
                                 HTTP_METHOD_POST,
                                 body,
                                 strlen(body),
                                 "application/json",
                                 response,
                                 sizeof(response));
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *parsed = cJSON_Parse(response);
    if (!parsed) {
        return ESP_FAIL;
    }
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(parsed, "file_id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    snprintf(file_id, file_id_size, "%s", id_item->valuestring);
    cJSON_Delete(parsed);

    return ESP_OK;
}

static esp_err_t upload_chunk(const char *file_id, uint32_t seq, const uint8_t *data, size_t data_len)
{
    char url[TELEMETRY_HTTP_URL_MAX];
    snprintf(url,
             sizeof(url),
             "%s/upload/%s/chunk/%" PRIu32,
             TELEMETRY_HTTP_UPLOAD_BASE_URL,
             file_id,
             seq);
    char response[TELEMETRY_HTTP_RESPONSE_MAX];
    esp_err_t err = ESP_OK;
    for (int attempt = 1; attempt <= TELEMETRY_HTTP_UPLOAD_RETRIES; attempt++) {
        err = http_request(url,
                           HTTP_METHOD_PUT,
                           data,
                           data_len,
                           "application/octet-stream",
                           response,
                           sizeof(response));
        if (err == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "HTTP chunk retry: file_id=%s seq=%" PRIu32 " attempt=%d/%d err=%s",
                 file_id,
                 seq,
                 attempt,
                 TELEMETRY_HTTP_UPLOAD_RETRIES,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_HTTP_UPLOAD_RETRY_DELAY_MS));
    }

    return err;
}

static esp_err_t upload_finish(const char *filename,
                               uint32_t request_id,
                               const char *file_id,
                               long long file_size,
                               uint32_t chunks_sent)
{
    char url[TELEMETRY_HTTP_URL_MAX];
    snprintf(url,
             sizeof(url),
             "%s/upload/%s/finish",
             TELEMETRY_HTTP_UPLOAD_BASE_URL,
             file_id);

    char body[128];
    snprintf(body,
             sizeof(body),
             "{\"expected_chunks\":%" PRIu32 ",\"expected_size\":%lld}",
             chunks_sent,
             file_size);

    char response[TELEMETRY_HTTP_RESPONSE_MAX];
    esp_err_t err = http_request(url,
                                 HTTP_METHOD_POST,
                                 body,
                                 strlen(body),
                                 "application/json",
                                 response,
                                 sizeof(response));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char csv_url[TELEMETRY_HTTP_URL_MAX];
    char bin_url[TELEMETRY_HTTP_URL_MAX];
    snprintf(csv_url, sizeof(csv_url), "%s/files/%s/csv", TELEMETRY_HTTP_UPLOAD_BASE_URL, file_id);
    snprintf(bin_url, sizeof(bin_url), "%s/files/%s/bin", TELEMETRY_HTTP_UPLOAD_BASE_URL, file_id);

    cJSON_AddStringToObject(root, "event", "upload_end");
    cJSON_AddStringToObject(root, "name", filename);
    cJSON_AddNumberToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "file_id", file_id);
    cJSON_AddNumberToObject(root, "bytes", file_size);
    cJSON_AddNumberToObject(root, "chunks", chunks_sent);
    cJSON_AddStringToObject(root, "csv_url", csv_url);
    cJSON_AddStringToObject(root, "bin_url", bin_url);
    (void)publish_download_event(root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t upload_raw_file(const telemetry_command_t *command,
                                 FILE *file,
                                 long long file_size,
                                 char *response,
                                 size_t response_size)
{
    if (file_size < 0 || file_size > INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = malloc(TELEMETRY_HTTP_UPLOAD_STREAM_BUFFER_BYTES);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    char url[TELEMETRY_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "%s/upload/raw", TELEMETRY_HTTP_UPLOAD_BASE_URL);
    if (response && response_size > 0) {
        response[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size_tx = TELEMETRY_HTTP_UPLOAD_TX_BUFFER_BYTES,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    char file_size_text[24];
    char request_id_text[16];
    snprintf(file_size_text, sizeof(file_size_text), "%lld", file_size);
    snprintf(request_id_text, sizeof(request_id_text), "%" PRIu32, command->request_id);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Device-Token", TELEMETRY_HTTP_UPLOAD_TOKEN);
    esp_http_client_set_header(client, "X-File-Name", command->filename);
    esp_http_client_set_header(client, "X-File-Size", file_size_text);
    esp_http_client_set_header(client, "X-Request-Id", request_id_text);

    ESP_LOGI(TAG, "HTTP STREAM POST %s size=%lld", url, file_size);
    esp_err_t err = esp_http_client_open(client, (int)file_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP stream open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buffer);
        return err;
    }

    long long uploaded = 0;
    long long next_progress_log = TELEMETRY_HTTP_PROGRESS_STEP_BYTES;
    long long next_yield = TELEMETRY_HTTP_UPLOAD_YIELD_STEP_BYTES;
    int64_t upload_started_us = esp_timer_get_time();
    while (1) {
        size_t read_len = fread(buffer, 1, TELEMETRY_HTTP_UPLOAD_STREAM_BUFFER_BYTES, file);
        if (read_len > 0) {
            size_t written_total = 0;
            while (written_total < read_len) {
                int written = esp_http_client_write(client,
                                                    (const char *)buffer + written_total,
                                                    read_len - written_total);
                if (written <= 0) {
                    ESP_LOGW(TAG,
                             "HTTP stream write failed: written=%d uploaded=%lld/%lld",
                             written,
                             uploaded,
                             file_size);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    free(buffer);
                    return ESP_FAIL;
                }
                written_total += (size_t)written;
                uploaded += written;
            }

            if (uploaded >= next_progress_log || uploaded == file_size) {
                int64_t elapsed_ms = (esp_timer_get_time() - upload_started_us) / 1000;
                long long bytes_per_s = elapsed_ms > 0 ? (uploaded * 1000LL) / elapsed_ms : 0;
                ESP_LOGI(TAG,
                         "HTTP stream progress: %lld/%lld bytes (%lld B/s)",
                         uploaded,
                         file_size,
                         bytes_per_s);
                publish_upload_progress(command->filename,
                                        command->request_id,
                                        "",
                                        uploaded,
                                        file_size);
                next_progress_log += TELEMETRY_HTTP_PROGRESS_STEP_BYTES;
            }
            if (uploaded >= next_yield || uploaded == file_size) {
                vTaskDelay(pdMS_TO_TICKS(1));
                next_yield += TELEMETRY_HTTP_UPLOAD_YIELD_STEP_BYTES;
            }
        }

        if (read_len < TELEMETRY_HTTP_UPLOAD_STREAM_BUFFER_BYTES) {
            if (ferror(file)) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                free(buffer);
                return ESP_FAIL;
            }
            break;
        }
    }

    free(buffer);
    buffer = NULL;

    int header_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int response_len = 0;
    if (response && response_size > 0) {
        response_len = esp_http_client_read_response(client, response, response_size - 1);
        if (response_len < 0) {
            response[0] = '\0';
        } else {
            response[response_len] = '\0';
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (header_len < 0) {
        ESP_LOGW(TAG, "HTTP stream response header failed: %d", header_len);
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "HTTP stream upload failed status=%d response=%s",
                 status,
                 response ? response : "");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP stream upload accepted status=%d response=%s", status, response ? response : "");
    return ESP_OK;
}

static void upload_log_file_http(const telemetry_command_t *command)
{
    FILE *file = NULL;
    char *response = NULL;
    long long file_size = 0;
    const char *error_reason = NULL;
    esp_err_t err = master_open_sd_file_for_download(command->filename, &file, &file_size);
    if (err != ESP_OK) {
        publish_download_error(command->filename, command->request_id, "open_failed", err);
        return;
    }

    s_download_active = true;

    char file_id[64] = {};
    response = calloc(1, TELEMETRY_HTTP_RESPONSE_MAX);
    if (!response) {
        err = ESP_ERR_NO_MEM;
        error_reason = "no_memory";
        goto cleanup;
    }

    err = upload_raw_file(command, file, file_size, response, TELEMETRY_HTTP_RESPONSE_MAX);
    if (err != ESP_OK) {
        error_reason = "upload_raw_failed";
        goto cleanup;
    }

    fclose(file);
    file = NULL;

    cJSON *parsed = cJSON_Parse(response);
    if (!parsed) {
        err = ESP_FAIL;
        error_reason = "upload_response_parse_failed";
        goto cleanup;
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(parsed, "file_id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring) {
        cJSON_Delete(parsed);
        err = ESP_FAIL;
        error_reason = "upload_response_missing_id";
        goto cleanup;
    }
    snprintf(file_id, sizeof(file_id), "%s", id_item->valuestring);

    const cJSON *csv_url_item = cJSON_GetObjectItemCaseSensitive(parsed, "csv_url");
    const cJSON *bin_url_item = cJSON_GetObjectItemCaseSensitive(parsed, "bin_url");
    char csv_url[TELEMETRY_HTTP_URL_MAX];
    char bin_url[TELEMETRY_HTTP_URL_MAX];
    if (cJSON_IsString(csv_url_item) && csv_url_item->valuestring) {
        if (strncmp(csv_url_item->valuestring, "http", 4) == 0) {
            snprintf(csv_url, sizeof(csv_url), "%s", csv_url_item->valuestring);
        } else {
            snprintf(csv_url, sizeof(csv_url), "%s%s", TELEMETRY_HTTP_UPLOAD_BASE_URL, csv_url_item->valuestring);
        }
    } else {
        snprintf(csv_url, sizeof(csv_url), "%s/files/%s/csv", TELEMETRY_HTTP_UPLOAD_BASE_URL, file_id);
    }
    if (cJSON_IsString(bin_url_item) && bin_url_item->valuestring) {
        if (strncmp(bin_url_item->valuestring, "http", 4) == 0) {
            snprintf(bin_url, sizeof(bin_url), "%s", bin_url_item->valuestring);
        } else {
            snprintf(bin_url, sizeof(bin_url), "%s%s", TELEMETRY_HTTP_UPLOAD_BASE_URL, bin_url_item->valuestring);
        }
    } else {
        snprintf(bin_url, sizeof(bin_url), "%s/files/%s/bin", TELEMETRY_HTTP_UPLOAD_BASE_URL, file_id);
    }
    cJSON_Delete(parsed);

    ESP_LOGI(TAG,
             "HTTP upload finished: name=%s id=%s bytes=%lld",
             command->filename,
             file_id,
             file_size);

    cJSON *complete = cJSON_CreateObject();
    if (!complete) {
        err = ESP_ERR_NO_MEM;
        error_reason = "no_memory";
        goto cleanup;
    }
    cJSON_AddStringToObject(complete, "event", "upload_end");
    cJSON_AddStringToObject(complete, "name", command->filename);
    cJSON_AddNumberToObject(complete, "request_id", command->request_id);
    cJSON_AddStringToObject(complete, "file_id", file_id);
    cJSON_AddNumberToObject(complete, "bytes", file_size);
    cJSON_AddStringToObject(complete, "csv_url", csv_url);
    cJSON_AddStringToObject(complete, "bin_url", bin_url);
    (void)publish_download_event(complete);
    cJSON_Delete(complete);

cleanup:
    if (response) {
        free(response);
    }
    if (file) {
        fclose(file);
    }
    s_download_active = false;
    if (error_reason) {
        ESP_LOGW(TAG,
                 "HTTP upload failed: name=%s id=%s reason=%s err=%s uploaded=%lld/%lld chunks=%" PRIu32,
                 command->filename,
                 file_id,
                 error_reason,
                 esp_err_to_name(err),
                 file_size,
                 file_size,
                 (uint32_t)0);
        publish_download_error(command->filename, command->request_id, error_reason, err);
    }
}

static bool should_ignore_backward_download_request(const telemetry_command_t *command)
{
    bool same_transfer =
        s_download_guard_request_id == command->request_id &&
        strcmp(s_download_guard_filename, command->filename) == 0;

    if (!same_transfer) {
        s_download_guard_request_id = command->request_id;
        s_download_guard_high_water_seq = command->start_seq;
        snprintf(s_download_guard_filename,
                 sizeof(s_download_guard_filename),
                 "%s",
                 command->filename);
        return false;
    }

    if (command->start_seq < s_download_guard_high_water_seq) {
        ESP_LOGW(TAG,
                 "Ignoring backward download command: %s request=%" PRIu32
                 " start_seq=%" PRIu32 " high_water=%" PRIu32,
                 command->filename,
                 command->request_id,
                 command->start_seq,
                 s_download_guard_high_water_seq);
        return true;
    }

    s_download_guard_high_water_seq = command->start_seq;
    return false;
}

static void publish_download_file(const telemetry_command_t *command)
{
    /*if (should_ignore_backward_download_request(command)) {
        return;
    }*/

    FILE *file = NULL;
    long long file_size = 0;
    esp_err_t err = master_open_sd_file_for_download(command->filename, &file, &file_size);
    if (err != ESP_OK) {
        publish_download_error(command->filename, command->request_id, "open_failed", err);
        return;
    }

    cJSON *begin = cJSON_CreateObject();
    if (!begin) {
        fclose(file);
        publish_download_error(command->filename, command->request_id, "no_memory", ESP_ERR_NO_MEM);
        return;
    }

    cJSON_AddStringToObject(begin, "event", "begin");
    cJSON_AddStringToObject(begin, "name", command->filename);
    cJSON_AddNumberToObject(begin, "request_id", command->request_id);
    cJSON_AddNumberToObject(begin, "size", file_size);
    cJSON_AddStringToObject(begin, "encoding", "base64");
    cJSON_AddNumberToObject(begin, "chunk_raw_bytes", TELEMETRY_DOWNLOAD_RAW_BYTES);
    uint32_t total_chunks = (uint32_t)(((uint64_t)file_size + TELEMETRY_DOWNLOAD_RAW_BYTES - 1u) /
                                      TELEMETRY_DOWNLOAD_RAW_BYTES);
    cJSON_AddNumberToObject(begin, "chunks", total_chunks);
    cJSON_AddNumberToObject(begin, "start_seq", command->start_seq);
    cJSON_AddNumberToObject(begin, "max_chunks", command->max_chunks);
    bool ok = publish_download_event(begin);
    cJSON_Delete(begin);

    if (!ok) {
        fclose(file);
        return;
    }

    s_download_active = true;
    uint8_t raw[TELEMETRY_DOWNLOAD_RAW_BYTES];
    uint8_t b64[TELEMETRY_DOWNLOAD_B64_BYTES + 1];
    size_t total_sent = 0;
    uint32_t seq = command->start_seq;
    uint32_t chunks_sent = 0;
    bool transfer_ok = true;
    bool window_complete = false;

    uint64_t offset = (uint64_t)command->start_seq * TELEMETRY_DOWNLOAD_RAW_BYTES;
    if (offset > (uint64_t)file_size) {
        fclose(file);
        s_download_active = false;
        publish_download_error(command->filename,
                               command->request_id,
                               "start_seq_past_end",
                               ESP_ERR_INVALID_ARG);
        return;
    }

    if (offset > 0 && fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        s_download_active = false;
        publish_download_error(command->filename,
                               command->request_id,
                               "seek_failed",
                               ESP_FAIL);
        return;
    }

    while (1) {
        if (command->max_chunks > 0 && chunks_sent >= command->max_chunks) {
            window_complete = true;
            break;
        }

        size_t n = fread(raw, 1, sizeof(raw), file);
        if (n > 0) {
            size_t olen = 0;
            int base64_err = mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, n);
            if (base64_err != 0) {
                publish_download_error(command->filename,
                                       command->request_id,
                                       "base64_encode_failed",
                                       ESP_FAIL);
                transfer_ok = false;
                break;
            }
            b64[olen] = '\0';

            cJSON *chunk = cJSON_CreateObject();
            if (!chunk) {
                publish_download_error(command->filename,
                                       command->request_id,
                                       "no_memory",
                                       ESP_ERR_NO_MEM);
                break;
            }

            cJSON_AddStringToObject(chunk, "event", "chunk");
            cJSON_AddStringToObject(chunk, "name", command->filename);
            cJSON_AddNumberToObject(chunk, "request_id", command->request_id);
            cJSON_AddNumberToObject(chunk, "seq", seq);
            cJSON_AddNumberToObject(chunk, "bytes", n);
            cJSON_AddStringToObject(chunk, "data", (const char *)b64);
            bool chunk_ok = publish_download_event(chunk);
            cJSON_Delete(chunk);

            if (!chunk_ok) {
                ESP_LOGW(TAG, "Failed to publish download chunk");
                publish_download_error(command->filename,
                                       command->request_id,
                                       "chunk_publish_failed",
                                       ESP_FAIL);
                transfer_ok = false;
                break;
            }

            total_sent += n;
            seq++;
            chunks_sent++;
        }

        if (n < sizeof(raw)) {
            if (ferror(file)) {
                publish_download_error(command->filename,
                                       command->request_id,
                                       "read_failed",
                                       ESP_FAIL);
                transfer_ok = false;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_DOWNLOAD_CHUNK_DELAY_MS));
    }

    fclose(file);
    s_download_active = false;

    if (!transfer_ok) {
        ESP_LOGW(TAG,
                 "Download incomplete: %s sent=%u expected=%lld",
                 command->filename,
                 (unsigned)total_sent,
                 file_size);
        return;
    }

    if (window_complete && seq < total_chunks) {
        cJSON *partial = cJSON_CreateObject();
        if (!partial) {
            publish_download_error(command->filename, command->request_id, "no_memory", ESP_ERR_NO_MEM);
            return;
        }

        cJSON_AddStringToObject(partial, "event", "partial");
        cJSON_AddStringToObject(partial, "name", command->filename);
        cJSON_AddNumberToObject(partial, "request_id", command->request_id);
        cJSON_AddNumberToObject(partial, "bytes", total_sent);
        cJSON_AddNumberToObject(partial, "chunks", total_chunks);
        cJSON_AddNumberToObject(partial, "next_seq", seq);
        (void)publish_download_event(partial);
        cJSON_Delete(partial);
        return;
    }

    cJSON *end = cJSON_CreateObject();
    if (!end) {
        publish_download_error(command->filename, command->request_id, "no_memory", ESP_ERR_NO_MEM);
        return;
    }

    cJSON_AddStringToObject(end, "event", "end");
    cJSON_AddStringToObject(end, "name", command->filename);
    cJSON_AddNumberToObject(end, "request_id", command->request_id);
    cJSON_AddNumberToObject(end, "bytes", file_size);
    cJSON_AddNumberToObject(end, "chunks", total_chunks);
    (void)publish_download_event(end);
    cJSON_Delete(end);
}

static void publish_config_ack(const char *event, esp_err_t result)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddBoolToObject(root, "ok", result == ESP_OK);
    cJSON_AddNumberToObject(root, "code", result);
    cJSON_AddStringToObject(root, "code_name", esp_err_to_name(result));
    (void)mqtt_publish_json_locked(TELEMETRY_CONFIG_TOPIC, root, 1);
    cJSON_Delete(root);
}

static void publish_config_save_ack(esp_err_t result, size_t bytes, bool apply)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "event", "save");
    cJSON_AddBoolToObject(root, "ok", result == ESP_OK);
    cJSON_AddNumberToObject(root, "code", result);
    cJSON_AddStringToObject(root, "code_name", esp_err_to_name(result));
    cJSON_AddNumberToObject(root, "bytes", (double)bytes);
    cJSON_AddBoolToObject(root, "apply", apply);
    (void)mqtt_publish_json_locked(TELEMETRY_CONFIG_TOPIC, root, 1);
    cJSON_Delete(root);
}

static void publish_config_text(void)
{
    char *config_text = NULL;
    bool from_sd = false;
    esp_err_t err = master_get_node_config_text(&config_text, &from_sd);
    if (err != ESP_OK) {
        publish_config_ack("get", err);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(config_text);
        publish_config_ack("get", ESP_ERR_NO_MEM);
        return;
    }

    cJSON_AddStringToObject(root, "event", "config");
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "source", from_sd ? "sd" : "generated");
    cJSON_AddStringToObject(root, "path", "/sdcard/nodes_config.json");
    cJSON_AddStringToObject(root, "config_text", config_text);
    (void)mqtt_publish_json_locked(TELEMETRY_CONFIG_TOPIC, root, 1);

    cJSON_Delete(root);
    free(config_text);
}

static void save_config_text(const telemetry_command_t *command)
{
    if (!command->config_text || command->config_text_len == 0) {
        publish_config_ack("save", ESP_ERR_INVALID_ARG);
        return;
    }

    esp_err_t err = master_save_node_config_text(command->config_text,
                                                 command->config_text_len,
                                                 command->apply_config);
    publish_config_save_ack(err, command->config_text_len, command->apply_config);
    if (err == ESP_OK) {
        publish_config_text();
    }
}

static void reload_config_text(void)
{
    esp_err_t err = master_reload_node_config();
    publish_config_ack("reload", err);
    if (err == ESP_OK) {
        publish_config_text();
    }
}

/* --------------------- Command worker task ------------------ */

static void command_task(void *arg)
{
    (void)arg;

    while (1) {
        telemetry_command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (command.type) {
        case TELEMETRY_COMMAND_START:
            ESP_LOGI(TAG, "Running MQTT start command");
            (void)master_start_logging();
            break;
        case TELEMETRY_COMMAND_STOP:
            ESP_LOGI(TAG, "Running MQTT stop command");
            (void)master_stop_logging();
            break;
        case TELEMETRY_COMMAND_FILES:
            ESP_LOGI(TAG, "Running MQTT files command");
            publish_files_telemetry();
            break;
        case TELEMETRY_COMMAND_DOWNLOAD:
            ESP_LOGI(TAG,
                     "Running HTTP upload command: %s request=%" PRIu32,
                     command.filename,
                     command.request_id);
            upload_log_file_http(&command);
            break;
        case TELEMETRY_COMMAND_CONFIG_GET:
            ESP_LOGI(TAG, "Running MQTT config_get command");
            publish_config_text();
            break;
        case TELEMETRY_COMMAND_CONFIG_SAVE:
            ESP_LOGI(TAG, "Running MQTT config_save command");
            save_config_text(&command);
            break;
        case TELEMETRY_COMMAND_CONFIG_RELOAD:
            ESP_LOGI(TAG, "Running MQTT config_reload command");
            reload_config_text();
            break;
        case TELEMETRY_COMMAND_LOG:
            ESP_LOGI(TAG, "Running MQTT log command: %s", command.log_mode);
            (void)master_set_log_mode_text(command.log_mode);
            break;
        case TELEMETRY_COMMAND_PREVIEW_CONFIG:
            portENTER_CRITICAL(&s_preview_mux);
            s_preview_can_id_count = command.preview_can_id_count;
            s_preview_all_samples = command.preview_all_samples;
            memcpy(s_preview_can_ids,
                   command.preview_can_ids,
                   sizeof(command.preview_can_ids));
            memset(s_preview_last_queue_time_us, 0, sizeof(s_preview_last_queue_time_us));
            portEXIT_CRITICAL(&s_preview_mux);
            ESP_LOGI(TAG,
                     "High-resolution CAN preview configured for %u ID(s), all_samples=%s",
                     (unsigned)command.preview_can_id_count,
                     command.preview_all_samples ? "true" : "false");
            break;
        default:
            break;
        }

        free_command_resources(&command);
    }
}

/* --------------------- Status, health, and live CAN publishers ------------------ */

static void publish_status_telemetry(char *json)
{
    if (master_format_status_json(json, TELEMETRY_JSON_MAX) == ESP_OK) {
        int msg_id = mqtt_publish_locked(TELEMETRY_STATUS_TOPIC, json, 1);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Published status telemetry msg_id=%d", msg_id);
        } else {
            ESP_LOGW(TAG, "Failed to publish status telemetry");
        }
    } else {
        ESP_LOGW(TAG, "Status JSON too large for telemetry");
    }
}

static bool is_node_health_frame(const telemetry_can_frame_t *frame)
{
    return frame &&
           frame->dlc >= 8 &&
           frame->id >= ID_NODE_HEALTH_BASE &&
           frame->id < (ID_NODE_HEALTH_BASE + 0x40);
}

static uint8_t frame_byte(const telemetry_can_frame_t *frame, uint8_t index)
{
    return (uint8_t)((frame->data >> (8 * index)) & 0xFFu);
}

static void publish_node_health_telemetry(const telemetry_can_frame_t *frame)
{
    if (!mqtt_is_connected() || !is_node_health_frame(frame)) {
        return;
    }

    uint8_t node_id = frame_byte(frame, 0);
    uint8_t flags = frame_byte(frame, 1);
    uint8_t load_percent = frame_byte(frame, 2);
    uint8_t max_lateness_ms = frame_byte(frame, 3);
    uint8_t sample_queue_depth = frame_byte(frame, 4);
    uint8_t missed_deadlines = frame_byte(frame, 5);
    uint8_t tx_fail_count = frame_byte(frame, 6);
    uint8_t free_heap_kb = frame_byte(frame, 7);
    bool active = (flags & 0x01u) != 0;
    bool sample_drops_seen = (flags & 0x02u) != 0;
    int64_t received_time_us = esp_timer_get_time();

    char json[TELEMETRY_HEALTH_JSON_MAX];
    int len = snprintf(json,
                       sizeof(json),
                       "{\"source\":\"node\",\"node_id\":%u,\"name\":\"node_%u\","
                       "\"health_can_id\":%" PRIu32 ",\"health_can_id_hex\":\"0x%03" PRIX32 "\","
                       "\"active\":%s,\"sample_drops_seen\":%s,"
                       "\"load_percent\":%u,\"task_load_percent\":%u,"
                       "\"cpu_load_percent\":%u,\"max_lateness_ms\":%u,"
                       "\"sample_queue_depth\":%u,\"missed_deadlines\":%u,"
                       "\"sample_queue_capacity\":%u,"
                       "\"tx_fail_count\":%u,\"free_heap_kb\":%u,"
                       "\"received_time_us\":%" PRId64 "}",
                       node_id,
                       node_id,
                       frame->id,
                       frame->id,
                       active ? "true" : "false",
                       sample_drops_seen ? "true" : "false",
                       load_percent,
                       load_percent,
                       load_percent,
                       max_lateness_ms,
                       sample_queue_depth,
                       missed_deadlines,
                       NODE_SAMPLE_QUEUE_CAPACITY,
                       tx_fail_count,
                       free_heap_kb,
                       received_time_us);

    if (len < 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "Node health JSON too large");
        return;
    }

    if (mqtt_publish_locked(TELEMETRY_HEALTH_TOPIC, json, 0) < 0) {
        ESP_LOGW(TAG, "Failed to publish node health telemetry");
    }
}

static void publish_master_health_telemetry(char *json)
{
    if (master_format_health_json(json, TELEMETRY_JSON_MAX) != ESP_OK) {
        ESP_LOGW(TAG, "Master health JSON too large for telemetry");
        return;
    }

    if (mqtt_publish_locked(TELEMETRY_HEALTH_TOPIC, json, 0) < 0) {
        ESP_LOGW(TAG, "Failed to publish master health telemetry");
    }
}

static void publish_gps_telemetry(const telemetry_gps_fix_t *fix)
{
    if (!fix || !mqtt_is_connected()) {
        return;
    }

    char lat[20] = "null";
    char lon[20] = "null";
    if (fix->has_location) {
        format_e7_decimal(fix->latitude_e7, lat, sizeof(lat));
        format_e7_decimal(fix->longitude_e7, lon, sizeof(lon));
    }

    char json[512];
    int len = snprintf(json,
                       sizeof(json),
                       "{\"source\":\"master\",\"sensor\":\"gps\","
                       "\"valid\":%s,\"has_location\":%s,"
                       "\"latitude\":%s,\"longitude\":%s,"
                       "\"latitude_e7\":%" PRId32 ",\"longitude_e7\":%" PRId32 ","
                       "\"altitude_cm\":%" PRId32 ","
                       "\"speed_kph_x100\":%u,\"course_deg_x100\":%u,"
                       "\"satellites\":%u,\"fix_quality\":%u,"
                       "\"hdop_x100\":%u,\"utc_time\":\"%s\",\"utc_date\":\"%s\","
                       "\"sample_time_us\":%" PRId64 "}",
                       fix->valid ? "true" : "false",
                       fix->has_location ? "true" : "false",
                       fix->has_location ? lat : "null",
                       fix->has_location ? lon : "null",
                       fix->latitude_e7,
                       fix->longitude_e7,
                       fix->altitude_cm,
                       fix->speed_kph_x100,
                       fix->course_deg_x100,
                       fix->satellites,
                       fix->fix_quality,
                       fix->hdop_x100,
                       fix->utc_time,
                       fix->utc_date,
                       fix->timestamp_us);
    if (len < 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "GPS telemetry JSON too large");
        return;
    }

    if (mqtt_publish_locked(TELEMETRY_GPS_TOPIC, json, 0) < 0) {
        ESP_LOGW(TAG, "Failed to publish GPS telemetry");
    }
}

static void publish_can_telemetry(const telemetry_can_frame_t *frame)
{
    if (!mqtt_is_connected()) {
        return;
    }

    if (is_node_health_frame(frame)) {
        publish_node_health_telemetry(frame);
        return;
    }

    char json[TELEMETRY_CAN_JSON_MAX];
    uint32_t value32 = (uint32_t)(frame->data & 0xFFFFFFFFu);
    uint32_t ts32 = (uint32_t)((frame->data >> 32) & 0xFFFFFFFFu);
    int64_t publish_time_us = esp_timer_get_time();

    int len = snprintf(json,
                       sizeof(json),
                       "{\"id\":%" PRIu32 ",\"id_hex\":\"0x%03" PRIX32 "\","
                       "\"dlc\":%u,\"timestamp\":%" PRIu32 ",\"value\":%" PRIu32 ","
                       "\"data\":%" PRIu64 ",\"publish_time_us\":%" PRId64 "}",
                       frame->id,
                       frame->id,
                       frame->dlc,
                       ts32,
                       value32,
                       frame->data,
                       publish_time_us);

    if (len < 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "CAN telemetry JSON too large");
        return;
    }

    int msg_id = mqtt_publish_locked(TELEMETRY_CAN_TOPIC, json, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish CAN telemetry");
    }
}

static void publish_can_batch_telemetry(char *json,
                                        size_t json_size,
                                        const telemetry_can_frame_t *frames,
                                        const bool *updated,
                                        size_t frame_count)
{
    if (!mqtt_is_connected() || !json || !frames || !updated) {
        return;
    }

    size_t pos = 0;
    bool have_can_frame = false;
    int64_t publish_time_us = esp_timer_get_time();

    if (!json_append(json, json_size, &pos, "{\"frames\":[")) {
        ESP_LOGW(TAG, "CAN batch JSON too large");
        return;
    }

    for (size_t i = 0; i < frame_count; i++) {
        if (!updated[i]) {
            continue;
        }

        const telemetry_can_frame_t *frame = &frames[i];
        if (is_node_health_frame(frame)) {
            publish_node_health_telemetry(frame);
            continue;
        }

        uint32_t value32 = (uint32_t)(frame->data & 0xFFFFFFFFu);
        uint32_t ts32 = (uint32_t)((frame->data >> 32) & 0xFFFFFFFFu);

        if (have_can_frame && !json_append(json, json_size, &pos, ",")) {
            ESP_LOGW(TAG, "CAN batch JSON too large");
            return;
        }

        if (!json_append(json,
                         json_size,
                         &pos,
                         "{\"id\":%" PRIu32 ",\"id_hex\":\"0x%03" PRIX32 "\","
                         "\"dlc\":%u,\"timestamp\":%" PRIu32 ",\"value\":%" PRIu32 ","
                         "\"data\":%" PRIu64 ",\"publish_time_us\":%" PRId64 "}",
                         frame->id,
                         frame->id,
                         frame->dlc,
                         ts32,
                         value32,
                         frame->data,
                         publish_time_us)) {
            ESP_LOGW(TAG, "CAN batch JSON too large");
            return;
        }

        have_can_frame = true;
    }

    if (!have_can_frame) {
        return;
    }

    if (!json_append(json,
                     json_size,
                     &pos,
                     "],\"publish_time_us\":%" PRId64 "}",
                     publish_time_us)) {
        ESP_LOGW(TAG, "CAN batch JSON too large");
        return;
    }

    int msg_id = mqtt_publish_locked(TELEMETRY_CAN_TOPIC, json, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish CAN telemetry batch");
    }
}

static bool high_resolution_preview_selected(uint32_t can_id)
{
    bool selected = false;

    portENTER_CRITICAL(&s_preview_mux);
    for (size_t i = 0; i < s_preview_can_id_count; i++) {
        if (s_preview_can_ids[i] == can_id) {
            selected = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_preview_mux);
    return selected;
}

static bool high_resolution_preview_sample_due(uint32_t can_id)
{
    int selected_slot = -1;

    portENTER_CRITICAL(&s_preview_mux);
    for (size_t i = 0; i < s_preview_can_id_count; i++) {
        if (s_preview_can_ids[i] == can_id) {
            selected_slot = (int)i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_preview_mux);

    if (selected_slot < 0) {
        return false;
    }

    bool due = false;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_preview_mux);
    if ((size_t)selected_slot < s_preview_can_id_count &&
        s_preview_can_ids[selected_slot] == can_id) {
        if (s_preview_all_samples) {
            due = true;
        } else if (s_preview_last_queue_time_us[selected_slot] == 0 ||
         (now_us - s_preview_last_queue_time_us[selected_slot]) >=
             TELEMETRY_CAN_HIGH_RES_INTERVAL_US) {
            s_preview_last_queue_time_us[selected_slot] = now_us;
            due = true;
        }
    }
    portEXIT_CRITICAL(&s_preview_mux);
    return due;
}

static void telemetry_task(void *arg)
{
    (void)arg;

    char *json = calloc(1, TELEMETRY_JSON_MAX);
    if (!json) {
        ESP_LOGE(TAG, "No memory for telemetry JSON buffer");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_status_tick = xTaskGetTickCount() - pdMS_TO_TICKS(TELEMETRY_PERIOD_MS);
    TickType_t last_can_tick = xTaskGetTickCount();
    TickType_t last_health_tick = xTaskGetTickCount() - pdMS_TO_TICKS(TELEMETRY_HEALTH_PERIOD_MS);
    telemetry_can_frame_t latest_can_frames[TELEMETRY_CAN_LATEST_SLOTS] = {};
    bool latest_can_valid[TELEMETRY_CAN_LATEST_SLOTS] = {};

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
                                               MQTT_CONNECTED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(TELEMETRY_CAN_PERIOD_MS));
        if ((bits & MQTT_CONNECTED_BIT) == 0) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if (!s_download_active &&
            (now - last_status_tick) >= pdMS_TO_TICKS(TELEMETRY_PERIOD_MS)) {
            publish_status_telemetry(json);
            last_status_tick = now;
        }

        if (!s_download_active &&
            (now - last_health_tick) >= pdMS_TO_TICKS(TELEMETRY_HEALTH_PERIOD_MS)) {
            publish_master_health_telemetry(json);
            last_health_tick = now;
        }

        if (!s_download_active) {
            telemetry_gps_fix_t gps_fix;
            while (s_gps_queue && xQueueReceive(s_gps_queue, &gps_fix, 0) == pdTRUE) {
                publish_gps_telemetry(&gps_fix);
            }

            telemetry_can_frame_t health_frame;
            while (s_health_queue &&
                   xQueueReceive(s_health_queue, &health_frame, 0) == pdTRUE) {
                publish_node_health_telemetry(&health_frame);
            }
        }

        if (!s_download_active &&
            (now - last_can_tick) >= pdMS_TO_TICKS(TELEMETRY_CAN_PERIOD_MS)) {
            bool latest_can_updated[TELEMETRY_CAN_LATEST_SLOTS] = {};

            telemetry_can_frame_t latest_frame;
            while (s_can_queue && xQueueReceive(s_can_queue, &latest_frame, 0) == pdTRUE) {
                if (is_node_health_frame(&latest_frame)) {
                    publish_node_health_telemetry(&latest_frame);
                    continue;
                }
                if (high_resolution_preview_selected(latest_frame.id)) {
                    continue;
                }

                int slot = -1;
                int free_slot = -1;

                for (int i = 0; i < TELEMETRY_CAN_LATEST_SLOTS; i++) {
                    if (latest_can_valid[i] && latest_can_frames[i].id == latest_frame.id) {
                        slot = i;
                        break;
                    }
                    if (!latest_can_valid[i] && free_slot < 0) {
                        free_slot = i;
                    }
                }

                if (slot < 0) {
                    slot = free_slot >= 0 ? free_slot : 0;
                }

                latest_can_frames[slot] = latest_frame;
                latest_can_valid[slot] = true;
                latest_can_updated[slot] = true;
            }

            publish_can_batch_telemetry(json,
                                        TELEMETRY_JSON_MAX,
                                        latest_can_frames,
                                        latest_can_updated,
                                        TELEMETRY_CAN_LATEST_SLOTS);

            telemetry_can_frame_t high_res_frames[TELEMETRY_CAN_HIGH_RES_BATCH_FRAMES];
            bool high_res_updated[TELEMETRY_CAN_HIGH_RES_BATCH_FRAMES];
            size_t high_res_total = 0;

            while (s_can_high_res_queue &&
                   high_res_total < TELEMETRY_CAN_HIGH_RES_MAX_PER_PERIOD) {
                size_t high_res_count = 0;
                while (high_res_count < TELEMETRY_CAN_HIGH_RES_BATCH_FRAMES &&
                       high_res_total < TELEMETRY_CAN_HIGH_RES_MAX_PER_PERIOD &&
                       xQueueReceive(s_can_high_res_queue,
                                     &high_res_frames[high_res_count],
                                     0) == pdTRUE) {
                    high_res_updated[high_res_count] = true;
                    high_res_count++;
                    high_res_total++;
                }

                if (high_res_count == 0) {
                    break;
                }

                publish_can_batch_telemetry(json,
                                            TELEMETRY_JSON_MAX,
                                            high_res_frames,
                                            high_res_updated,
                                            high_res_count);
            }

            last_can_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* --------------------- Public telemetry API ------------------ */

void telemetry_submit_can_frame(uint32_t id, uint8_t dlc, uint64_t data)
{
    if (!s_can_queue) {
        return;
    }

    telemetry_can_frame_t frame = {
        .id = id,
        .dlc = dlc,
        .data = data,
    };

    QueueHandle_t queue = is_node_health_frame(&frame) ? s_health_queue : s_can_queue;
    if (!queue) {
        return;
    }

    if (!is_node_health_frame(&frame) &&
        s_can_high_res_queue &&
        high_resolution_preview_sample_due(id)) {
        if (xQueueSend(s_can_high_res_queue, &frame, 0) != pdPASS) {
            telemetry_can_frame_t high_res_dropped;
            (void)xQueueReceive(s_can_high_res_queue, &high_res_dropped, 0);
            (void)xQueueSend(s_can_high_res_queue, &frame, 0);
        }
    }

    if (xQueueSend(queue, &frame, 0) == pdPASS) {
        return;
    }

    telemetry_can_frame_t dropped;
    (void)xQueueReceive(queue, &dropped, 0);
    if (xQueueSend(queue, &frame, 0) != pdPASS) {
        ESP_LOGW(TAG,
                 "%s telemetry queue full - newest frame dropped",
                 is_node_health_frame(&frame) ? "Health" : "CAN");
    }
}

void telemetry_submit_gps_fix(const telemetry_gps_fix_t *fix)
{
    if (!fix || !s_gps_queue) {
        return;
    }

    (void)xQueueOverwrite(s_gps_queue, fix);
}

esp_err_t telemetry_start(void)
{
    if (s_mqtt_client != NULL) {
        return ESP_OK;
    }

    s_mqtt_event_group = xEventGroupCreate();
    if (!s_mqtt_event_group) {
        return ESP_ERR_NO_MEM;
    }

    s_can_queue = xQueueCreate(TELEMETRY_CAN_QUEUE_LENGTH, sizeof(telemetry_can_frame_t));
    if (!s_can_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_can_high_res_queue = xQueueCreate(TELEMETRY_CAN_HIGH_RES_QUEUE_LENGTH,
                                       sizeof(telemetry_can_frame_t));
    if (!s_can_high_res_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_health_queue = xQueueCreate(TELEMETRY_HEALTH_QUEUE_LENGTH, sizeof(telemetry_can_frame_t));
    if (!s_health_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_gps_queue = xQueueCreate(TELEMETRY_GPS_QUEUE_LENGTH, sizeof(telemetry_gps_fix_t));
    if (!s_gps_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_command_queue = xQueueCreate(TELEMETRY_COMMAND_QUEUE_LENGTH, sizeof(telemetry_command_t));
    if (!s_command_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_publish_mutex = xSemaphoreCreateMutex();
    if (!s_publish_mutex) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(s_mqtt_client_id,
             sizeof(s_mqtt_client_id),
             "%s_%08" PRIx32,
             TELEMETRY_MQTT_CLIENT_ID_PREFIX,
             esp_random());
    load_mqtt_broker_uri(s_mqtt_broker_uri, sizeof(s_mqtt_broker_uri));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_broker_uri,
        .credentials.client_id = s_mqtt_client_id,
        .session.keepalive = 10,
        .network.timeout_ms = TELEMETRY_MQTT_NETWORK_TIMEOUT_MS,
        .network.reconnect_timeout_ms = TELEMETRY_MQTT_RECONNECT_TIMEOUT_MS,
        .buffer.size = TELEMETRY_MQTT_BUFFER_BYTES,
        .buffer.out_size = TELEMETRY_MQTT_BUFFER_BYTES,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));

    BaseType_t task_ok = xTaskCreate(telemetry_task,
                                     "telemetry",
                                     TELEMETRY_TASK_STACK_BYTES,
                                     NULL,
                                     5,
                                     &s_telemetry_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }

    task_ok = xTaskCreate(command_task,
                          "mqtt_cmd",
                          TELEMETRY_COMMAND_TASK_STACK_BYTES,
                          NULL,
                          6,
                          &s_command_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT command task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Telemetry started. Broker=%s status_topic=%s can_topic=%s health_topic=%s command_topic=%s",
             s_mqtt_broker_uri,
             TELEMETRY_STATUS_TOPIC,
             TELEMETRY_CAN_TOPIC,
             TELEMETRY_HEALTH_TOPIC,
             TELEMETRY_COMMAND_TOPIC);
    return ESP_OK;
}
