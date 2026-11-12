#include "health/node_health.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "live/live_data.h"
#include "logger/data_logger.h"
#include "node_state/node_registry.h"

/* The master reports itself as node 0: other modules count problems here and health_task logs them. */

static uint8_t resets;
static uint8_t reset_reason;

/**
 * @brief Get the master time in ms.
 *
 * @return Milliseconds since boot.
 */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Get the base ID of a health frame's kind.
 *
 * @param id CAN ID.
 * @return The status, event or diagnostics base ID; status for lower IDs.
 */
static uint32_t frame_base(uint32_t id)
{
    if (id >= CAN_ID_NODE_DIAGNOSTICS_BASE) return CAN_ID_NODE_DIAGNOSTICS_BASE;
    return id >= CAN_ID_NODE_EVENT_BASE ? CAN_ID_NODE_EVENT_BASE : CAN_ID_NODE_STATUS_BASE;
}

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
    [HEALTH_SAMPLES_MISSED] = "samples_missed",
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
    "adc_node_6", "front_shock_node_7", "rear_shock_node_8",
};
static const char *const sensor_state_names[] = {"ok", "some_readings_failed", "no_readings",
                                                 "failed_to_start"};

/**
 * @brief Name a reset reason.
 *
 * @param reason Reset reason code from a diagnostics frame.
 * @return The name, or "unknown".
 */
static const char *reset_name(uint8_t reason)
{
    return reason < sizeof(reset_names) / sizeof(reset_names[0]) ? reset_names[reason] : "unknown";
}

const char *node_health_name_level(node_health_t level)
{
    return health_names[level];
}

const char *node_health_name_sensor_state(sensor_state_t state)
{
    return sensor_state_names[state];
}

const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size)
{
    if (id >= CAN_ID_NODE_STATUS_BASE && id < CAN_ID_NODE_STATUS_BASE + CAN_NODE_SLOT_COUNT) {
        node_status_frame_t status;
        memcpy(&status, &data, sizeof(status));
        uint8_t node_id = (uint8_t)(id - CAN_ID_NODE_STATUS_BASE);
        *node = node_names[node_id];
        size_t length = (size_t)snprintf(text, size, "%s %s", node_health_name_level(status.health),
            status.mode == NODE_MODE_RECORDING ? "recording" : "idle");
        /* Faults newer than this build still show, by number. */
        for (int fault = 0; fault < HEALTH_STATUS_FAULT_BITS && length < size; fault++) {
            if (!(status.faults & HEALTH_BIT(fault))) {
                continue;
            }
            if (fault < HEALTH_FAULT_COUNT && code_names[fault]) {
                length += (size_t)snprintf(text + length, size - length, " %s", code_names[fault]);
            } else {
                length += (size_t)snprintf(text + length, size - length, " fault_%d", fault);
            }
        }
        for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS && length < size; slot++) {
            uint32_t sensor = protocol_slot_sensor_id(node_id, slot);
            sensor_state_t state = node_status_sensor_state(&status, slot);
            if (sensor && state != SENSOR_STATE_OK) {
                length += (size_t)snprintf(text + length, size - length, " 0x%02X %s",
                                           (unsigned)sensor, node_health_name_sensor_state(state));
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
            for (int slot = 1; slot < CAN_NODE_SLOT_COUNT && length < size; slot++) {
                if (event.subject & protocol_offline_node_bit((uint8_t)slot)) {
                    length += (size_t)snprintf(text + length, size - length, " %s",
                                               node_names[slot]);
                }
            }
            return "health_event";
        }
        char subject[32];
        if (event.code == HEALTH_BOOT) {
            snprintf(subject, sizeof(subject), "reset %s", reset_name(event.subject));
        } else if (event.code == HEALTH_ADC_UNCALIBRATED) {
            snprintf(subject, sizeof(subject), "gpio %u", (unsigned)event.subject);
        } else if (event.code == HEALTH_EVENT_LOST) {
            snprintf(subject, sizeof(subject), "node %s",
                     event.subject < CAN_NODE_SLOT_COUNT ? node_names[event.subject] : "unknown");
        } else {
            snprintf(subject, sizeof(subject), "subject 0x%02X",
                     (unsigned)protocol_health_subject_id(event.subject));
        }
        snprintf(text, size, "%s %s count %u%s %s",
                 event.code < HEALTH_CODE_COUNT && code_names[event.code] ? code_names[event.code] : "unknown",
                 subject, event.count,
                 event.count == HEALTH_EVENT_COUNT_MAX ? "+" : "",
                 event.error <= HEALTH_ERROR_OTHER ? error_names[event.error] : "unknown");
        return "health_event";
    }
    if (id >= CAN_ID_NODE_DIAGNOSTICS_BASE &&
        id < CAN_ID_NODE_DIAGNOSTICS_BASE + CAN_NODE_SLOT_COUNT) {
        node_diagnostics_frame_t diagnostics;
        memcpy(&diagnostics, &data, sizeof(diagnostics));
        *node = node_names[id - CAN_ID_NODE_DIAGNOSTICS_BASE];
        snprintf(text, size, "tx_errors %u rx_errors %u resets %u last_reset %s",
                 diagnostics.tx_errors, diagnostics.rx_errors, diagnostics.resets,
                 reset_name(diagnostics.reset_reason));
        return "node_diagnostics";
    }
    return NULL;
}

#define SUMMARY_FRESH_MS 3000

typedef struct {
    node_status_frame_t status;     /* timestamp_ms holds the arrival time */
    node_diagnostics_frame_t diagnostics;
    bool status_seen;
} node_summary_t;
static node_summary_t summary[CAN_NODE_SLOT_COUNT];
static portMUX_TYPE summary_lock = portMUX_INITIALIZER_UNLOCKED;

bool node_health_get(uint8_t node, node_health_view_t *view)
{
    if (node >= CAN_NODE_SLOT_COUNT) return false;
    portENTER_CRITICAL(&summary_lock);
    node_summary_t entry = summary[node];
    portEXIT_CRITICAL(&summary_lock);
    *view = (node_health_view_t){
        .name = node_names[node],
        .fresh = entry.status_seen && now_ms() - entry.status.timestamp_ms < SUMMARY_FRESH_MS,
        .level = entry.status.health,
        .faults = entry.status.faults,
        .resets = entry.diagnostics.resets,
        .last_reset = reset_name(entry.diagnostics.reset_reason),
    };
    size_t count = 0;
    const live_signal_metadata_t *signals = live_data_signals(&count);
    for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS; slot++) {
        uint32_t id = protocol_slot_sensor_id(node, slot);
        if (!id) break;
        view->sensors[slot].name = "unknown";
        for (size_t i = 0; i < count; i++) {
            if (signals[i].can_id == id) view->sensors[slot].name = signals[i].signal;
        }
        view->sensors[slot].state = node_status_sensor_state(&entry.status, slot);
        view->sensor_count = slot + 1;
    }
    return true;
}

static const char *const fault_labels[HEALTH_FAULT_COUNT] = {
    [HEALTH_SAMPLES_MISSED] = "sampling fell behind",
    [HEALTH_SAMPLES_DROPPED] = "samples were lost before sending",
    [HEALTH_BAD_TIMESTAMP] = "samples have bad times",
    [HEALTH_NO_TIME_SYNC] = "no clock sync from the master",
    [HEALTH_CAN_TX] = "CAN sends failed",
    [HEALTH_CAN_RX_OVERFLOW] = "CAN receive overflowed",
    [HEALTH_CAN_NO_ACK] = "no other node answers on CAN",
    [HEALTH_CAN_BUS_ERROR] = "CAN wiring errors",
    [HEALTH_CAN_PASSIVE] = "too many CAN errors",
    [HEALTH_CAN_BUS_OFF] = "off the CAN bus",
    [HEALTH_SD_WRITE] = "SD card write failed",
    [HEALTH_GPS_NO_DATA] = "no data from the GPS",
    [HEALTH_GPS_NO_FIX] = "GPS has no fix",
    [HEALTH_EVENT_LOST] = "health reports were lost",
};

const char *node_health_label_fault(health_code_t fault)
{
    return fault < HEALTH_FAULT_COUNT ? fault_labels[fault] : NULL;
}

static const char *const sensor_state_labels[] = {"ok", "some readings failed", "no readings",
                                                  "failed to start"};

const char *node_health_label_sensor_state(sensor_state_t state)
{
    return sensor_state_labels[state];
}

/**
 * @brief Describe a node's problems in one line, led by its severity.
 *
 * A restart makes an ok node a warning. Sensors all in the same bad state show
 * as "all sensors".
 *
 * @param view Node health from node_health_get().
 * @param expected true if a stale node counts as not responding.
 * @param text Filled with the line, such as "critical: engine_node_5:
 *             engine_rpm: failed to start".
 * @param size Size of text in bytes.
 * @return false if there is nothing to report.
 */
static bool describe_node(const node_health_view_t *view, bool expected, char *text, size_t size)
{
    if (!view->fresh) {
        if (expected) snprintf(text, size, "critical: %s: not responding", view->name);
        return expected;
    }
    if (view->level == NODE_HEALTH_OK && !view->resets) return false;
    node_health_t level = view->level;
    if (view->resets && level < NODE_HEALTH_WARNING) level = NODE_HEALTH_WARNING;
    size_t length = (size_t)snprintf(text, size, "%s: %s:", node_health_name_level(level), view->name);
    const char *separator = " ";
    bool all = view->sensor_count >= 2;
    for (size_t i = 1; i < view->sensor_count; i++) {
        if (view->sensors[i].state != view->sensors[0].state) all = false;
    }
    for (size_t i = 0; i < (all ? 1 : view->sensor_count) && length < size; i++) {
        if (view->sensors[i].state == SENSOR_STATE_OK) continue;
        length += (size_t)snprintf(text + length, size - length, "%s%s: %s", separator,
                                   all ? "all sensors" : view->sensors[i].name,
                                   node_health_label_sensor_state(view->sensors[i].state));
        separator = "; ";
    }
    for (int fault = 0; fault < HEALTH_STATUS_FAULT_BITS && length < size; fault++) {
        if (!(view->faults & HEALTH_BIT(fault))) continue;
        const char *label = node_health_label_fault(fault);
        if (label) {
            length += (size_t)snprintf(text + length, size - length, "%s%s", separator, label);
        } else {
            length += (size_t)snprintf(text + length, size - length, "%sfault %d", separator, fault);
        }
        separator = "; ";
    }
    if (view->resets && length < size) {
        snprintf(text + length, size - length, "%srestarted %u time%s, last: %s", separator,
                 (unsigned)view->resets, view->resets == 1 ? "" : "s", view->last_reset);
    }
    return true;
}

void node_health_list_problems(char *out, size_t size)
{
    size_t length = 0;
    out[0] = '\0';
    for (uint8_t node = 0; node < CAN_NODE_SLOT_COUNT; node++) {
        node_health_view_t view;
        char text[NODE_HEALTH_TEXT_MAX];
        bool expected = (MASTER_EXPECTED_NODE_MASK >> node) & 1;
        if (!node_health_get(node, &view) || !describe_node(&view, expected, text, sizeof(text))) continue;
        size_t room = size - length;
        size_t written = (size_t)snprintf(out + length, room, "%s\n", text);
        if (written >= room) {
            out[length] = '\0';
            break;
        }
        length += written;
    }
}

/* Used only from CAN dispatch. */
static uint8_t last_sequence[CAN_NODE_SLOT_COUNT];
static bool sequence_seen[CAN_NODE_SLOT_COUNT];

/**
 * @brief Check a node event's sequence number against the last one.
 *
 * A boot event restarts the check.
 *
 * @param node Sending node.
 * @param event Received event.
 * @param missed Set to the number of earlier events that never arrived.
 * @return false for a repeated event, which CAN can deliver twice.
 */
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
    bool event_frame = (base == CAN_ID_NODE_EVENT_BASE);
    if (message->dlc < (event_frame ? 8 : NODE_HEALTH_PAYLOAD)) return false;
    can_message_t frame = *message;
    frame.dlc = 8;
    unsigned node = message->id - base;
    bool node_event = event_frame && node != 0;
    uint32_t missed = 0;
    if (!event_frame) {
        frame.data = (uint32_t)frame.data | (uint64_t)now_ms() << 32;
        node_summary_t *entry = &summary[node];
        portENTER_CRITICAL(&summary_lock);
        if (base == CAN_ID_NODE_STATUS_BASE) {
            memcpy(&entry->status, &frame.data, sizeof(entry->status));
            entry->status_seen = true;
        } else {
            uint8_t resets = entry->diagnostics.resets;
            memcpy(&entry->diagnostics, &frame.data, sizeof(entry->diagnostics));
            if (entry->diagnostics.resets != resets) sequence_seen[node] = false;
        }
        portEXIT_CRITICAL(&summary_lock);
    } else if (node_event) {
        health_event_frame_t event;
        memcpy(&event, &frame.data, sizeof(event));
        if (!check_sequence(node, &event, &missed)) return true;
    }
    bool logged = data_logger_enqueue_health(&frame);
    /* The node counts a sent event as delivered, so a refused one is lost. */
    if (node_event && !logged) missed++;
    node_health_count(HEALTH_EVENT_LOST, missed, node, ESP_OK);
    return logged;
}

/**
 * @brief Log one of the master's own health frames. See node_health_handle_frame().
 *
 * @param id Base ID of the frame kind; node 0 is the master.
 * @param frame 8 bytes of the frame kind's struct.
 * @return true if the frame was logged.
 */
static bool log_own_frame(uint32_t id, const void *frame)
{
    can_message_t message = {.id = id, .dlc = 8};
    memcpy(&message.data, frame, 8);
    return node_health_handle_frame(&message);
}

/**
 * @brief Log the master's CAN error counters, resets and last reset reason.
 *
 * Error counters above 255 are sent as 255.
 */
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

/**
 * @brief Log one event per counter that grew since its last report.
 *
 * Stops at the first event the logger refuses; the rest wait for the next call.
 *
 * @param now Current counts.
 * @param reported Counts already reported; updated per logged event.
 */
static void log_events(const health_state_t *now, health_state_t *reported)
{
    for (size_t i = 0; i < HEALTH_COUNTER_COUNT; i++) {
        const health_counter_t *counter = &now->counters[i];
        uint32_t count = counter->count - reported->counters[i].count;
        if (count == 0) continue;
        health_event_frame_t event = node_health_pack_event(counter->code, counter->subject, 0, count,
                                                            counter->error, counter->last_ms);
        if (!log_own_frame(CAN_ID_NODE_EVENT_BASE, &event)) return;
        reported->counters[i].count = counter->count;
    }
}

/**
 * @brief Task that reports the master's health. See node_health_init().
 */
static void health_task(void *argument)
{
    (void)argument;
    /* Static to keep three copies of the state off the task stack. */
    static health_state_t now, previous, reported;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(NODE_STATUS_PERIOD_MS));
        node_health_copy_counts(&now);
        /* Status covers only the last period; events carry everything else. */
        uint32_t faults = node_health_get_faults(&now, &previous);
        previous = now;
        bool recording = (data_logger_state() == LOGGER_RUNNING);
        node_status_frame_t status = {
            .faults = faults,
            .health = health_for(faults, SENSOR_STATE_OK, false),
            .mode = recording ? NODE_MODE_RECORDING : NODE_MODE_IDLE,
        };
        log_own_frame(CAN_ID_NODE_STATUS_BASE, &status);
        log_diagnostics();
        if (recording) log_events(&now, &reported);
    }
}

esp_err_t node_health_init(void)
{
    node_health_count_boot(&resets, &reset_reason);
    return xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
