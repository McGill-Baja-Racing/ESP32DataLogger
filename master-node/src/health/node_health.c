#include "health/node_health.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
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
 * Node status, event and diagnostics frames are printed and logged like
 * samples. The master reports itself the same way, as node 0: other modules
 * count problems here and health_task sends them once a second. Printing
 * also happens in health_task: the serial console is slow enough to stall
 * CAN dispatch.
 */

#define PRINT_QUEUE_LENGTH 32
#define SHARED_COUNTERS 32
#define COUNTER_COUNT (HEALTH_CODE_COUNT + SHARED_COUNTERS)
#define RESET_COUNT_MAGIC 0xB4A7DA03u

/* Counts are cumulative, so unsigned differences survive wrap-around. */
typedef struct {
    uint8_t code;
    uint32_t count;
    uint32_t subject;
    esp_err_t error;
    uint32_t last_ms;
} counter_t;

/* Copied whole under the lock so a status and its events agree. */
typedef struct {
    counter_t counters[COUNTER_COUNT];
    bool active[HEALTH_FAULT_COUNT];
} health_state_t;

static const char *TAG = "Health";
static health_state_t state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t print_queue;
/* Last printed status [0] and diagnostics [1] of each node. */
static uint32_t last_printed[2][CAN_NODE_SLOT_COUNT];
static bool printed[2][CAN_NODE_SLOT_COUNT];
/* Kept through every reset but power-on; the magic tells a kept count from
 * power-on garbage. */
static RTC_NOINIT_ATTR uint32_t reset_count_magic;
static RTC_NOINIT_ATTR uint32_t reset_count;
static uint8_t resets;
static uint8_t reset_reason;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool counted_per_subject(health_code_t code)
{
    return code == HEALTH_SAMPLES_LATE || code == HEALTH_SAMPLES_DROPPED ||
           code == HEALTH_BAD_TIMESTAMP || code == HEALTH_CAN_TX ||
           code == HEALTH_CAN_RX_OVERFLOW || code == HEALTH_EVENT_LOST;
}

/* Counter N belongs to code N. A code counted per subject keeps its first
 * subject there and takes shared counters for the others. Shared counters are
 * taken in order and never freed, so the first unused one ends the search;
 * once all are taken, a new subject is added to the code's own counter.
 * Call under lock. */
static counter_t *counter_for(health_code_t code, uint32_t subject)
{
    counter_t *own = &state.counters[code];
    if (!counted_per_subject(code) || own->count == 0 || own->subject == subject) {
        return own;
    }
    for (size_t i = HEALTH_CODE_COUNT; i < COUNTER_COUNT; i++) {
        counter_t *counter = &state.counters[i];
        if (counter->count == 0 || (counter->code == code && counter->subject == subject)) {
            return counter;
        }
    }
    return own;
}

void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error)
{
    if (amount == 0) return;
    subject = protocol_health_subject(subject);
    uint32_t now = now_ms();
    portENTER_CRITICAL_SAFE(&lock);
    counter_t *counter = counter_for(code, subject);
    counter->code = code;
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

static uint32_t frame_base(uint32_t id)
{
    if (id >= CAN_ID_NODE_DIAGNOSTICS_BASE) return CAN_ID_NODE_DIAGNOSTICS_BASE;
    return id >= CAN_ID_NODE_EVENT_BASE ? CAN_ID_NODE_EVENT_BASE : CAN_ID_NODE_STATUS_BASE;
}

/* Node 0 is the master itself, never a frame from the bus. */
bool node_health_is_frame(const can_message_t *message)
{
    uint32_t base = frame_base(message->id);
    return message->id > base && message->id < base + CAN_NODE_SLOT_COUNT;
}

uint8_t node_health_node_id(const can_message_t *message)
{
    return (uint8_t)(message->id - frame_base(message->id));
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
    [HEALTH_EVENT_LOST] = "event_lost",
    [HEALTH_BOOT] = "boot",
    [HEALTH_ADC_UNCALIBRATED] = "adc_uncalibrated",
    [HEALTH_NODE_OFFLINE] = "node_offline",
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
/* Indexed by esp_reset_reason_t. */
static const char *const reset_names[] = {
    "unknown", "power_on", "external", "software", "panic", "interrupt_watchdog",
    "task_watchdog", "watchdog", "deep_sleep", "brownout", "sdio", "usb", "jtag",
    "efuse", "power_glitch", "cpu_lockup",
};
static const char *const node_names[CAN_NODE_SLOT_COUNT] = {
    "master", "brake_node_1", "node_2", "mpu_node_3", "encoder_node_4", "engine_node_5",
    "adc_node_6",
};

const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size)
{
    if (id >= CAN_ID_NODE_STATUS_BASE && id < CAN_ID_NODE_STATUS_BASE + CAN_NODE_SLOT_COUNT) {
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
    if (id >= CAN_ID_NODE_EVENT_BASE && id < CAN_ID_NODE_EVENT_BASE + CAN_NODE_SLOT_COUNT) {
        health_event_frame_t event;
        memcpy(&event, &data, sizeof(event));
        *node = node_names[id - CAN_ID_NODE_EVENT_BASE];
        if (event.code == HEALTH_NODE_OFFLINE) {
            size_t length = (size_t)snprintf(text, size, "node_offline");
            for (int slot = 0; slot < CAN_NODE_SLOT_COUNT && length < size; slot++) {
                if (event.subject & (1U << slot)) {
                    length += (size_t)snprintf(text + length, size - length, " %s",
                                               node_names[slot]);
                }
            }
            return "health_event";
        }
        snprintf(text, size, "%s subject 0x%02X count %u%s %s",
                 event.code < HEALTH_CODE_COUNT ? code_names[event.code] : "unknown",
                 (unsigned)protocol_health_subject_id(event.subject), event.count,
                 event.count == HEALTH_EVENT_COUNT_MAX ? "+" : "",
                 event.error <= HEALTH_ERROR_OTHER ? error_names[event.error] : "unknown");
        return "health_event";
    }
    if (id >= CAN_ID_NODE_DIAGNOSTICS_BASE &&
        id < CAN_ID_NODE_DIAGNOSTICS_BASE + CAN_NODE_SLOT_COUNT) {
        node_diagnostics_frame_t diagnostics;
        memcpy(&diagnostics, &data, sizeof(diagnostics));
        *node = node_names[id - CAN_ID_NODE_DIAGNOSTICS_BASE];
        size_t reason_count = sizeof(reset_names) / sizeof(reset_names[0]);
        snprintf(text, size, "tx_errors %u rx_errors %u resets %u last_reset %s",
                 diagnostics.tx_errors, diagnostics.rx_errors, diagnostics.resets,
                 diagnostics.reset_reason < reason_count
                     ? reset_names[diagnostics.reset_reason] : "unknown");
        return "node_diagnostics";
    }
    return NULL;
}

/* Prints every event, but status and diagnostics only when they change. */
static void print_frame(const can_message_t *message)
{
    uint32_t base = frame_base(message->id);
    if (base != CAN_ID_NODE_EVENT_BASE) {
        int kind = base == CAN_ID_NODE_DIAGNOSTICS_BASE;
        unsigned node = message->id - base;
        uint32_t summary = (uint32_t)message->data;
        if (printed[kind][node] && summary == last_printed[kind][node]) return;
        printed[kind][node] = true;
        last_printed[kind][node] = summary;
    }
    const char *node;
    char text[NODE_HEALTH_TEXT_MAX];
    node_health_describe(message->id, message->data, &node, text, sizeof(text));
    ESP_LOGI(TAG, "%s %s", node, text);
}

/* Used only from CAN dispatch. */
static uint8_t last_sequence[CAN_NODE_SLOT_COUNT];
static bool sequence_seen[CAN_NODE_SLOT_COUNT];

/* Sets *missed to the node's events before this one that never arrived.
 * Returns false for a repeated sequence: CAN delivers a frame twice when only
 * its sender saw an error in the last bit. */
static bool check_sequence(unsigned node, const health_event_frame_t *event,
                           uint32_t *missed)
{
    *missed = 0;
    if (sequence_seen[node] && event->code != HEALTH_BOOT) {
        if (event->sequence == last_sequence[node]) return false;
        *missed = (event->sequence - last_sequence[node] - 1u) & HEALTH_EVENT_SEQUENCE_MASK;
    }
    sequence_seen[node] = true;
    last_sequence[node] = event->sequence;
    return true;
}

bool node_health_handle_frame(const can_message_t *message)
{
    uint32_t base = frame_base(message->id);
    bool event_frame = base == CAN_ID_NODE_EVENT_BASE;
    if (message->dlc < (event_frame ? 8 : NODE_HEALTH_PAYLOAD)) return false;
    can_message_t frame = *message;
    frame.dlc = 8;
    unsigned node = message->id - base;
    bool node_event = event_frame && node != 0;
    uint32_t missed = 0;
    if (!event_frame) {
        frame.data = (uint32_t)frame.data | (uint64_t)now_ms() << 32;
    } else if (node_event) {
        health_event_frame_t event;
        memcpy(&event, &frame.data, sizeof(event));
        if (!check_sequence(node, &event, &missed)) return true;
    }
    if (print_queue) (void)xQueueSend(print_queue, &frame, 0);
    bool logged = data_logger_enqueue_health(&frame);
    /* The node counts a sent event as delivered, so a refused one is lost. */
    if (node_event && !logged) missed++;
    node_health_count(HEALTH_EVENT_LOST, missed, node, ESP_OK);
    return logged;
}

static bool log_own_frame(uint32_t id, const void *frame)
{
    can_message_t message = {.id = id, .dlc = 8};
    memcpy(&message.data, frame, 8);
    return node_health_handle_frame(&message);
}

static void log_diagnostics(void)
{
    uint32_t rx_errors = 0;
    uint32_t tx_errors = 0;
    (void)can_master_error_counts(&rx_errors, &tx_errors);
    node_diagnostics_frame_t diagnostics = {
        .tx_errors = tx_errors > UINT8_MAX ? UINT8_MAX : (uint8_t)tx_errors,
        .rx_errors = rx_errors > UINT8_MAX ? UINT8_MAX : (uint8_t)rx_errors,
        .resets = resets,
        .reset_reason = reset_reason,
    };
    log_own_frame(CAN_ID_NODE_DIAGNOSTICS_BASE, &diagnostics);
}

static void log_events(const health_state_t *now, health_state_t *reported)
{
    for (size_t i = 0; i < COUNTER_COUNT; i++) {
        const counter_t *counter = &now->counters[i];
        uint32_t count = counter->count - reported->counters[i].count;
        if (count == 0) continue;
        if (count > HEALTH_EVENT_COUNT_MAX) count = HEALTH_EVENT_COUNT_MAX;
        health_event_frame_t event = {
            .code = counter->code,
            .subject = (uint8_t)counter->subject,
            .count = count,
            .error = health_error(counter->error),
            .timestamp_ms = counter->last_ms,
        };
        /* Not logged, for example at stop: keep it for the next run. */
        if (!log_own_frame(CAN_ID_NODE_EVENT_BASE, &event)) return;
        reported->counters[i].count = counter->count;
    }
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
        uint32_t faults = 0;
        for (int fault = 0; fault < HEALTH_FAULT_COUNT; fault++) {
            if (now.active[fault]) faults |= HEALTH_BIT(fault);
        }
        for (size_t i = 0; i < COUNTER_COUNT; i++) {
            if (now.counters[i].count != previous.counters[i].count &&
                now.counters[i].code < HEALTH_FAULT_COUNT) {
                faults |= HEALTH_BIT(now.counters[i].code);
            }
        }
        previous = now;
        bool recording = data_logger_state() == LOGGER_RUNNING;
        node_status_frame_t status = {
            .faults = faults,
            .health = health_for(faults, false),
            .mode = recording ? NODE_MODE_RECORDING : NODE_MODE_IDLE,
        };
        log_own_frame(CAN_ID_NODE_STATUS_BASE, &status);
        log_diagnostics();
        if (recording) log_events(&now, &reported);
    }
}

esp_err_t node_health_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reset_count_magic != RESET_COUNT_MAGIC) {
        reset_count_magic = RESET_COUNT_MAGIC;
        reset_count = 0;
    } else if (reset_count < UINT8_MAX) {
        reset_count++;
    }
    resets = (uint8_t)reset_count;
    reset_reason = (uint8_t)reason;
    node_health_count(HEALTH_BOOT, 1, reason, ESP_OK);
    print_queue = xQueueCreate(PRINT_QUEUE_LENGTH, sizeof(can_message_t));
    if (!print_queue) return ESP_ERR_NO_MEM;
    return xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
