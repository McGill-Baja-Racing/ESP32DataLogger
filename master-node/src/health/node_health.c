#include "health/node_health.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logger/data_logger.h"

/*
 * Node health
 * -----------
 * Node status and event frames are printed and logged unchanged, like
 * samples. The master reports itself the same way, as node 0: other modules
 * count problems here and health_task sends them once a second. Printing
 * also happens in health_task: the serial console is slow enough to stall
 * CAN dispatch.
 */

#define NODE_SLOT_COUNT 7
#define PRINT_QUEUE_LENGTH 32

/* Counts are cumulative, so unsigned differences survive wrap-around. */
typedef struct {
    uint32_t count;
    uint32_t subject;
    esp_err_t error;
    uint32_t last_ms;
} counter_t;

/* Copied whole under the lock so a status and its events agree. */
typedef struct {
    counter_t counters[HEALTH_CODE_COUNT];
    bool active[HEALTH_FAULT_COUNT];
} health_state_t;

static const char *TAG = "Health";
static health_state_t state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t print_queue;
static uint32_t last_status[NODE_SLOT_COUNT];
static bool status_printed[NODE_SLOT_COUNT];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error)
{
    if (amount == 0) return;
    uint32_t now = now_ms();
    counter_t *counter = &state.counters[code];
    portENTER_CRITICAL_SAFE(&lock);
    counter->count += amount;
    counter->subject = subject;
    counter->error = error;
    counter->last_ms = now;
    portEXIT_CRITICAL_SAFE(&lock);
}

void node_health_set(health_code_t fault, bool active)
{
    portENTER_CRITICAL_SAFE(&lock);
    bool onset = active && !state.active[fault];
    state.active[fault] = active;
    portEXIT_CRITICAL_SAFE(&lock);
    /* Counting each onset keeps a fault shorter than one status period visible. */
    if (onset) node_health_count(fault, 1, 0, ESP_OK);
}

static health_error_t health_error(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return HEALTH_ERROR_NONE;
    case ESP_FAIL: return HEALTH_ERROR_FAIL;
    case ESP_ERR_NO_MEM: return HEALTH_ERROR_NO_MEM;
    case ESP_ERR_INVALID_ARG: return HEALTH_ERROR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return HEALTH_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return HEALTH_ERROR_NOT_FOUND;
    case ESP_ERR_TIMEOUT: return HEALTH_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_RESPONSE: return HEALTH_ERROR_INVALID_RESPONSE;
    default: return HEALTH_ERROR_OTHER;
    }
}

bool node_health_is_frame(const can_message_t *message)
{
    return (message->id > CAN_ID_NODE_STATUS_BASE &&
            message->id < CAN_ID_NODE_STATUS_BASE + NODE_SLOT_COUNT) ||
           (message->id > CAN_ID_NODE_EVENT_BASE &&
            message->id < CAN_ID_NODE_EVENT_BASE + NODE_SLOT_COUNT);
}

uint8_t node_health_node_id(const can_message_t *message)
{
    return (uint8_t)(message->id - (message->id < CAN_ID_NODE_EVENT_BASE
                                    ? CAN_ID_NODE_STATUS_BASE : CAN_ID_NODE_EVENT_BASE));
}

static const char *const code_names[HEALTH_CODE_COUNT] = {
    [HEALTH_SENSOR_INIT] = "sensor_init",
    [HEALTH_SENSOR_READ] = "sensor_read",
    [HEALTH_SENSOR_DEAD] = "sensor_dead",
    [HEALTH_SAMPLES_LATE] = "samples_late",
    [HEALTH_SAMPLES_DROPPED] = "samples_dropped",
    [HEALTH_BAD_TIMESTAMP] = "bad_timestamp",
    [HEALTH_NO_TIME_SYNC] = "no_time_sync",
    [HEALTH_CAN_TX] = "can_tx",
    [HEALTH_CAN_RX_OVERFLOW] = "can_rx_overflow",
    [HEALTH_CAN_NO_ACK] = "can_no_ack",
    [HEALTH_CAN_BUS_ERROR] = "can_bus_error",
    [HEALTH_CAN_PASSIVE] = "can_passive",
    [HEALTH_CAN_BUS_OFF] = "can_bus_off",
    [HEALTH_SD_WRITE] = "sd_write",
    [HEALTH_GPS_NO_DATA] = "gps_no_data",
    [HEALTH_GPS_NO_FIX] = "gps_no_fix",
    [HEALTH_BOOT] = "boot",
    [HEALTH_ADC_UNCALIBRATED] = "adc_uncalibrated",
};
static const char *const error_names[] = {
    [HEALTH_ERROR_NONE] = "ESP_OK",
    [HEALTH_ERROR_FAIL] = "ESP_FAIL",
    [HEALTH_ERROR_NO_MEM] = "ESP_ERR_NO_MEM",
    [HEALTH_ERROR_INVALID_ARG] = "ESP_ERR_INVALID_ARG",
    [HEALTH_ERROR_INVALID_STATE] = "ESP_ERR_INVALID_STATE",
    [HEALTH_ERROR_NOT_FOUND] = "ESP_ERR_NOT_FOUND",
    [HEALTH_ERROR_TIMEOUT] = "ESP_ERR_TIMEOUT",
    [HEALTH_ERROR_INVALID_RESPONSE] = "ESP_ERR_INVALID_RESPONSE",
    [HEALTH_ERROR_OTHER] = "other error",
};
static const char *const health_names[] = {"ok", "warning", "error", "critical"};
static const char *const node_names[NODE_SLOT_COUNT] = {
    "master", "node_1", "node_2", "node_3", "node_4", "node_5", "node_6",
};

const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size)
{
    if (id >= CAN_ID_NODE_STATUS_BASE && id < CAN_ID_NODE_STATUS_BASE + NODE_SLOT_COUNT) {
        node_status_frame_t status;
        memcpy(&status, &data, sizeof(status));
        *node = node_names[id - CAN_ID_NODE_STATUS_BASE];
        size_t length = (size_t)snprintf(text, size, "%s %s",
            status.health <= NODE_HEALTH_CRITICAL ? health_names[status.health] : "unknown",
            status.mode == NODE_MODE_RECORDING ? "recording" : "idle");
        for (int fault = 0; fault < HEALTH_FAULT_COUNT && length < size; fault++) {
            if (status.faults & HEALTH_BIT(fault)) {
                length += (size_t)snprintf(text + length, size - length, " %s",
                                           code_names[fault]);
            }
        }
        return "node_status";
    }
    if (id >= CAN_ID_NODE_EVENT_BASE && id < CAN_ID_NODE_EVENT_BASE + NODE_SLOT_COUNT) {
        health_event_frame_t event;
        memcpy(&event, &data, sizeof(event));
        *node = node_names[id - CAN_ID_NODE_EVENT_BASE];
        snprintf(text, size, "%s subject 0x%02X count %u %s",
                 event.code < HEALTH_CODE_COUNT ? code_names[event.code] : "unknown",
                 event.subject, event.count,
                 event.error <= HEALTH_ERROR_OTHER ? error_names[event.error] : "unknown");
        return "health_event";
    }
    return NULL;
}

/* Prints every event, but a status frame only when it changes. */
static void print_frame(const can_message_t *message)
{
    if (message->id < CAN_ID_NODE_EVENT_BASE) {
        unsigned node = message->id - CAN_ID_NODE_STATUS_BASE;
        uint32_t summary = (uint32_t)message->data;
        if (status_printed[node] && summary == last_status[node]) return;
        status_printed[node] = true;
        last_status[node] = summary;
    }
    const char *node;
    char text[NODE_HEALTH_TEXT_MAX];
    node_health_describe(message->id, message->data, &node, text, sizeof(text));
    ESP_LOGI(TAG, "%s %s", node, text);
}

bool node_health_handle_frame(const can_message_t *message)
{
    if (message->dlc != 8) return false;
    can_message_t frame = *message;
    if (frame.id < CAN_ID_NODE_EVENT_BASE) {
        node_status_frame_t status;
        memcpy(&status, &frame.data, sizeof(status));
        /* Without beacons a node stamps status with its own clock. */
        if (status.faults & HEALTH_BIT(HEALTH_NO_TIME_SYNC)) {
            status.timestamp_ms = now_ms();
            memcpy(&frame.data, &status, sizeof(status));
        }
    }
    if (print_queue) (void)xQueueSend(print_queue, &frame, 0);
    return data_logger_enqueue(&frame);
}

static bool log_own_frame(uint32_t id, const void *frame)
{
    can_message_t message = {.id = id, .dlc = 8};
    memcpy(&message.data, frame, 8);
    return node_health_handle_frame(&message);
}

static void health_task(void *argument)
{
    (void)argument;
    /* Static to keep three copies of the state off the task stack. */
    static health_state_t now, previous, reported;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(NODE_STATUS_PERIOD_MS));
        can_message_t frame;
        while (xQueueReceive(print_queue, &frame, 0) == pdTRUE) print_frame(&frame);

        portENTER_CRITICAL(&lock);
        now = state;
        portEXIT_CRITICAL(&lock);
        /* Status covers only the last period; events carry everything else. */
        uint16_t faults = 0;
        for (int fault = 0; fault < HEALTH_FAULT_COUNT; fault++) {
            if (now.active[fault] ||
                now.counters[fault].count != previous.counters[fault].count) {
                faults |= HEALTH_BIT(fault);
            }
        }
        previous = now;
        bool recording = data_logger_state() == LOGGER_RUNNING;
        node_status_frame_t status = {
            .faults = faults,
            .health = health_for(faults, false),
            .mode = recording ? NODE_MODE_RECORDING : NODE_MODE_IDLE,
            .timestamp_ms = now_ms(),
        };
        log_own_frame(CAN_ID_NODE_STATUS_BASE, &status);
        if (!recording) continue;

        for (int code = 0; code < HEALTH_CODE_COUNT; code++) {
            const counter_t *counter = &now.counters[code];
            uint32_t count = counter->count - reported.counters[code].count;
            if (count == 0) continue;
            health_event_frame_t event = {
                .code = code,
                .subject = (uint8_t)counter->subject,
                .count = count > UINT8_MAX ? UINT8_MAX : (uint8_t)count,
                .error = health_error(counter->error),
                .timestamp_ms = counter->last_ms,
            };
            /* Not logged, for example at stop: keep it for the next run. */
            if (!log_own_frame(CAN_ID_NODE_EVENT_BASE, &event)) break;
            reported.counters[code].count = counter->count;
        }
    }
}

esp_err_t node_health_init(void)
{
    node_health_count(HEALTH_BOOT, 1, esp_reset_reason(), ESP_OK);
    print_queue = xQueueCreate(PRINT_QUEUE_LENGTH, sizeof(can_message_t));
    if (!print_queue) return ESP_ERR_NO_MEM;
    return xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
