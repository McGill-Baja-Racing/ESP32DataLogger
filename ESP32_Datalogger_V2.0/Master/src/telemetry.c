#include "telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mqtt_client.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "master_control.h"

static const char *TAG = "telemetry";

#define TELEMETRY_MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define TELEMETRY_MQTT_CLIENT_ID_PREFIX "baja_logger_master"
#define TELEMETRY_STATUS_TOPIC    "baja/logger/master/status"
#define TELEMETRY_CAN_TOPIC       "baja/logger/master/can"
#define TELEMETRY_COMMAND_TOPIC   "baja/logger/master/command"
#define TELEMETRY_FILES_TOPIC     "baja/logger/master/files"
#define TELEMETRY_DOWNLOAD_TOPIC  "baja/logger/master/download"
#define TELEMETRY_PERIOD_MS       5000
#define TELEMETRY_CAN_PERIOD_MS    100
#define TELEMETRY_JSON_MAX        4096
#define TELEMETRY_CAN_JSON_MAX     192
#define TELEMETRY_CAN_QUEUE_LENGTH  64
#define TELEMETRY_COMMAND_QUEUE_LENGTH 4
#define TELEMETRY_COMMAND_PAYLOAD_MAX 192
#define TELEMETRY_DOWNLOAD_FILENAME_MAX 64
#define TELEMETRY_DOWNLOAD_RAW_BYTES 192
#define TELEMETRY_DOWNLOAD_B64_BYTES (((TELEMETRY_DOWNLOAD_RAW_BYTES + 2) / 3) * 4)
#define TELEMETRY_DOWNLOAD_CHUNK_DELAY_MS 5

#define MQTT_CONNECTED_BIT BIT0

typedef enum {
    TELEMETRY_COMMAND_START = 0,
    TELEMETRY_COMMAND_STOP,
    TELEMETRY_COMMAND_FILES,
    TELEMETRY_COMMAND_DOWNLOAD,
} telemetry_command_type_t;

typedef struct {
    telemetry_command_type_t type;
    char filename[TELEMETRY_DOWNLOAD_FILENAME_MAX];
    uint32_t request_id;
} telemetry_command_t;

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint64_t data;
} telemetry_can_frame_t;

static EventGroupHandle_t s_mqtt_event_group;
static QueueHandle_t s_can_queue;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_publish_mutex;
static esp_mqtt_client_handle_t s_mqtt_client;
static TaskHandle_t s_telemetry_task_handle;
static TaskHandle_t s_command_task_handle;
static char s_mqtt_client_id[48];

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

static int mqtt_publish_locked(const char *topic, const char *payload, int qos)
{
    if (!s_mqtt_client || !s_publish_mutex) {
        return -1;
    }

    if (xSemaphoreTake(s_publish_mutex, portMAX_DELAY) != pdTRUE) {
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

static void queue_command_request(const telemetry_command_t *command)
{
    if (!s_command_queue) {
        ESP_LOGW(TAG, "MQTT command queue not ready");
        return;
    }

    if (xQueueSend(s_command_queue, command, 0) == pdPASS) {
        return;
    }

    telemetry_command_t dropped;
    (void)xQueueReceive(s_command_queue, &dropped, 0);
    if (xQueueSend(s_command_queue, command, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT command queue full - newest command dropped");
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

    return false;
}

static void handle_command_payload(const esp_mqtt_event_handle_t event)
{
    if (!event || !event->data || event->data_len <= 0) {
        return;
    }

    if (event->data_len >= TELEMETRY_COMMAND_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "MQTT command payload too large");
        return;
    }

    char payload[TELEMETRY_COMMAND_PAYLOAD_MAX] = {};
    memcpy(payload, event->data, event->data_len);
    payload[event->data_len] = '\0';

    cJSON *root = cJSON_Parse(payload);
    if (root) {
        const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        if (cJSON_IsString(cmd_item) && cmd_item->valuestring) {
            char command_text[TELEMETRY_COMMAND_PAYLOAD_MAX] = {};
            snprintf(command_text, sizeof(command_text), "%s", cmd_item->valuestring);

            telemetry_command_t command = {};
            if (!fill_command_from_text(command_text, &command)) {
                ESP_LOGW(TAG, "Unknown MQTT command: %s", command_text);
                cJSON_Delete(root);
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
                    return;
                }

                snprintf(command.filename, sizeof(command.filename), "%s", file_item->valuestring);
            }

            cJSON_Delete(root);
            queue_command_request(&command);
            return;
        }

        cJSON_Delete(root);
        ESP_LOGW(TAG, "MQTT command JSON missing string field: cmd");
        return;
    }

    telemetry_command_t command = {};
    if (fill_command_from_text(payload, &command)) {
        if (command.type == TELEMETRY_COMMAND_DOWNLOAD && command.filename[0] == '\0') {
            ESP_LOGW(TAG, "MQTT download command missing file");
            return;
        }
        queue_command_request(&command);
    } else {
        ESP_LOGW(TAG, "Unknown MQTT command: %s", payload);
    }
}

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
        }
        break;
    default:
        break;
    }
}

static void publish_files_telemetry(void)
{
    char *json = calloc(1, TELEMETRY_JSON_MAX);
    if (!json) {
        ESP_LOGE(TAG, "No memory for files JSON buffer");
        return;
    }

    if (master_format_files_json(json, TELEMETRY_JSON_MAX) == ESP_OK) {
        if (mqtt_publish_locked(TELEMETRY_FILES_TOPIC, json, 1) < 0) {
            ESP_LOGW(TAG, "Failed to publish files telemetry");
        }
    } else {
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "files_json_too_large");
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

static void publish_download_file(const telemetry_command_t *command)
{
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
    bool ok = publish_download_event(begin);
    cJSON_Delete(begin);

    if (!ok) {
        fclose(file);
        return;
    }

    uint8_t raw[TELEMETRY_DOWNLOAD_RAW_BYTES];
    uint8_t b64[TELEMETRY_DOWNLOAD_B64_BYTES + 1];
    size_t total_sent = 0;
    uint32_t seq = 0;

    while (1) {
        size_t n = fread(raw, 1, sizeof(raw), file);
        if (n > 0) {
            size_t olen = 0;
            int base64_err = mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, raw, n);
            if (base64_err != 0) {
                publish_download_error(command->filename,
                                       command->request_id,
                                       "base64_encode_failed",
                                       ESP_FAIL);
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
                break;
            }

            total_sent += n;
            seq++;
        }

        if (n < sizeof(raw)) {
            if (ferror(file)) {
                publish_download_error(command->filename,
                                       command->request_id,
                                       "read_failed",
                                       ESP_FAIL);
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_DOWNLOAD_CHUNK_DELAY_MS));
    }

    fclose(file);

    cJSON *end = cJSON_CreateObject();
    if (!end) {
        publish_download_error(command->filename, command->request_id, "no_memory", ESP_ERR_NO_MEM);
        return;
    }

    cJSON_AddStringToObject(end, "event", "end");
    cJSON_AddStringToObject(end, "name", command->filename);
    cJSON_AddNumberToObject(end, "request_id", command->request_id);
    cJSON_AddNumberToObject(end, "bytes", total_sent);
    cJSON_AddNumberToObject(end, "chunks", seq);
    (void)publish_download_event(end);
    cJSON_Delete(end);
}

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
            ESP_LOGI(TAG, "Running MQTT download command: %s", command.filename);
            publish_download_file(&command);
            break;
        default:
            break;
        }
    }
}

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

static void publish_can_telemetry(const telemetry_can_frame_t *frame)
{
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
        if ((now - last_status_tick) >= pdMS_TO_TICKS(TELEMETRY_PERIOD_MS)) {
            publish_status_telemetry(json);
            last_status_tick = now;
        }

        if ((now - last_can_tick) >= pdMS_TO_TICKS(TELEMETRY_CAN_PERIOD_MS)) {
            telemetry_can_frame_t latest_frame;
            bool have_frame = false;

            while (s_can_queue && xQueueReceive(s_can_queue, &latest_frame, 0) == pdTRUE) {
                have_frame = true;
            }

            if (have_frame) {
                publish_can_telemetry(&latest_frame);
            }

            last_can_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

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

    if (xQueueSend(s_can_queue, &frame, 0) == pdPASS) {
        return;
    }

    telemetry_can_frame_t dropped;
    (void)xQueueReceive(s_can_queue, &dropped, 0);
    if (xQueueSend(s_can_queue, &frame, 0) != pdPASS) {
        ESP_LOGW(TAG, "CAN telemetry queue full - newest frame dropped");
    }
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

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = TELEMETRY_MQTT_BROKER_URI,
        .credentials.client_id = s_mqtt_client_id,
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
                                     4096,
                                     NULL,
                                     5,
                                     &s_telemetry_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }

    task_ok = xTaskCreate(command_task,
                          "mqtt_cmd",
                          4096,
                          NULL,
                          6,
                          &s_command_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT command task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Telemetry started. Broker=%s status_topic=%s can_topic=%s command_topic=%s",
             TELEMETRY_MQTT_BROKER_URI,
             TELEMETRY_STATUS_TOPIC,
             TELEMETRY_CAN_TOPIC,
             TELEMETRY_COMMAND_TOPIC);
    return ESP_OK;
}
