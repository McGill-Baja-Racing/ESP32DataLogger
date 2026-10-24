#include "health/node_health.h"

#include <inttypes.h>

#include "can/can_node.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sampler/sampler.h"
#include "sensors/sensor.h"
#include "time/time_sync.h"

/*
 * Node health
 * -----------
 * Other modules only count problems here. Once a second health_task sends a
 * status and a diagnostics frame and, while recording, one event per counted
 * code and subject since the last report. A count is marked reported once the
 * CAN driver holds its event, so problems while idle are sent later, stamped
 * with when they last happened; an event the driver then drops shows up at the
 * master as a gap in the event sequence. A count above HEALTH_EVENT_COUNT_MAX
 * is sent as that maximum, so a fault storm while idle leaves no backlog. Occurrence times are kept on the local clock
 * because counting runs in interrupts; they are converted when sent.
 */

#define SHARED_COUNTERS      24
#define COUNTER_COUNT        (HEALTH_CODE_COUNT + SHARED_COUNTERS)
#define TIME_SYNC_TIMEOUT_MS 1000
#define RESET_COUNT_MAGIC    0xB4A7DA03u

/* One report sends at most one event per sensor and counter. With 64 or more,
 * a whole lost report could wrap the sequence unseen. */
_Static_assert(MAX_SENSORS + COUNTER_COUNT < HEALTH_EVENT_SEQUENCE_MASK + 1,
               "a report must fit the event sequence");

/* Counts are cumulative, so unsigned differences survive wrap-around. */
typedef struct {
    uint8_t code;
    uint32_t count;
    uint32_t subject;
    esp_err_t error;
    uint32_t last_ms;
} counter_t;

typedef struct {
    uint32_t can_id;
    esp_err_t init_error;
    uint32_t init_ms;
    uint32_t good_reads;
    uint32_t failed_reads;
    esp_err_t last_error;
    uint32_t last_failure_ms;
} sensor_health_t;

/* Copied whole under the lock so a status and its events agree. */
typedef struct {
    counter_t counters[COUNTER_COUNT];
    sensor_health_t sensors[MAX_SENSORS];
    size_t sensor_count;
    bool active[HEALTH_FAULT_COUNT];
} health_state_t;

static const char *TAG = "Health";
static health_state_t state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
/* Kept through every reset but power-on; the magic tells a kept count from
 * power-on garbage. */
static RTC_NOINIT_ATTR uint32_t reset_count_magic;
static RTC_NOINIT_ATTR uint32_t reset_count;
static uint8_t resets;
static uint8_t reset_reason;
static uint8_t event_sequence;

static uint32_t local_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void node_health_sensor_init(size_t index, uint32_t can_id, esp_err_t error)
{
    if (index >= MAX_SENSORS) {
        return;
    }
    uint32_t now = local_ms();
    portENTER_CRITICAL_SAFE(&lock);
    state.sensors[index].can_id = can_id;
    state.sensors[index].init_error = error;
    state.sensors[index].init_ms = now;
    if (index >= state.sensor_count) {
        state.sensor_count = index + 1;
    }
    portEXIT_CRITICAL_SAFE(&lock);
}

void node_health_sensor_read(size_t index, esp_err_t error)
{
    if (index >= MAX_SENSORS) {
        return;
    }
    uint32_t now = local_ms();
    sensor_health_t *sensor = &state.sensors[index];
    portENTER_CRITICAL_SAFE(&lock);
    if (error == ESP_OK) {
        sensor->good_reads++;
    } else {
        sensor->failed_reads++;
        sensor->last_error = error;
        sensor->last_failure_ms = now;
    }
    portEXIT_CRITICAL_SAFE(&lock);
}

static bool counted_per_subject(health_code_t code)
{
    return code == HEALTH_SAMPLES_LATE || code == HEALTH_SAMPLES_DROPPED ||
           code == HEALTH_BAD_TIMESTAMP || code == HEALTH_CAN_TX ||
           code == HEALTH_CAN_RX_OVERFLOW;
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
    if (amount == 0) {
        return;
    }
    uint32_t now = local_ms();
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
    if (onset) {
        node_health_count(fault, 1, 0, ESP_OK);
    }
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

static health_event_frame_t event_frame(uint8_t code, uint32_t subject, uint8_t sequence,
                                        uint32_t count, esp_err_t error, int32_t timestamp_ms)
{
    health_event_frame_t event = {
        .code = code,
        .subject = (uint8_t)subject,
        .sequence = sequence & HEALTH_EVENT_SEQUENCE_MASK,
        .count = count > HEALTH_EVENT_COUNT_MAX ? HEALTH_EVENT_COUNT_MAX : count,
        .error = health_error(error),
        .timestamp_ms = (uint32_t)timestamp_ms,
    };
    return event;
}

static bool send_event(uint8_t code, uint32_t subject, uint32_t count, esp_err_t error,
                       uint32_t when_local_ms)
{
    /* A sent event counts as delivered, so send only while the master is
     * logging: recording, with its beacons still arriving. */
    if (!sampler_is_active() || !time_sync_is_current(TIME_SYNC_TIMEOUT_MS)) {
        return false;
    }
    /* Problems from before the master's clock started are dated at its start. */
    int32_t when_ms = time_sync_timestamp_ms_at((int64_t)when_local_ms * 1000);
    health_event_frame_t event = event_frame(code, subject, event_sequence, count, error,
                                             when_ms < 0 ? 0 : when_ms);
    if (can_node_queue(CAN_ID_NODE_EVENT_BASE + NODE_ID, (const uint8_t *)&event, sizeof(event)) != ESP_OK) {
        return false;
    }
    event_sequence++;
    ESP_LOGI(TAG, "event %u subject 0x%02" PRIX32 " count %" PRIu32 " %s",
             code, subject, count, esp_err_to_name(error));
    return true;
}

static bool report_counter(const health_state_t *now, health_state_t *reported, size_t i)
{
    const counter_t *counter = &now->counters[i];
    uint32_t count = counter->count - reported->counters[i].count;
    if (count && !send_event(counter->code, counter->subject, count, counter->error,
                             counter->last_ms)) {
        return false;
    }
    reported->counters[i].count = counter->count;
    return true;
}

/* Stops at the first unsent event: on a failing bus each send holds the
 * transmitter long enough to delay samples. */
static void report_events(const health_state_t *now, health_state_t *reported,
                          bool *init_reported)
{
    /* The master restarts its sequence check at the boot event. */
    if (!report_counter(now, reported, HEALTH_BOOT)) {
        return;
    }
    for (size_t i = 0; i < now->sensor_count; i++) {
        const sensor_health_t *sensor = &now->sensors[i];
        if (sensor->init_error != ESP_OK) {
            if (!init_reported[i]) {
                if (!send_event(HEALTH_SENSOR_INIT, sensor->can_id, 1,
                                sensor->init_error, sensor->init_ms)) {
                    return;
                }
                init_reported[i] = true;
            }
            continue;
        }
        init_reported[i] = false;
        uint32_t failed = sensor->failed_reads - reported->sensors[i].failed_reads;
        if (failed && !send_event(HEALTH_SENSOR_READ, sensor->can_id, failed,
                                  sensor->last_error, sensor->last_failure_ms)) {
            return;
        }
        reported->sensors[i].failed_reads = sensor->failed_reads;
    }
    for (size_t i = 0; i < COUNTER_COUNT; i++) {
        if (!report_counter(now, reported, i)) {
            return;
        }
    }
}

static void send_diagnostics(void)
{
    uint8_t tx_errors = 0;
    uint8_t rx_errors = 0;
    can_node_error_counts(&tx_errors, &rx_errors);
    node_diagnostics_frame_t diagnostics = {
        .tx_errors = tx_errors,
        .rx_errors = rx_errors,
        .resets = resets,
        .reset_reason = reset_reason,
    };
    (void)can_node_send(CAN_ID_NODE_DIAGNOSTICS_BASE + NODE_ID, (const uint8_t *)&diagnostics,
                        NODE_HEALTH_PAYLOAD);
}

static void health_task(void *argument)
{
    (void)argument;
    /* Static to keep three copies of the state off the task stack. */
    static health_state_t now, previous, reported;
    static bool init_reported[MAX_SENSORS];
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(NODE_STATUS_PERIOD_MS));
        portENTER_CRITICAL(&lock);
        now = state;
        portEXIT_CRITICAL(&lock);

        /* Status covers only the last period; events carry everything else. */
        uint32_t faults = 0;
        for (int fault = 0; fault < HEALTH_FAULT_COUNT; fault++) {
            if (now.active[fault]) {
                faults |= HEALTH_BIT(fault);
            }
        }
        for (size_t i = 0; i < COUNTER_COUNT; i++) {
            if (now.counters[i].count != previous.counters[i].count &&
                now.counters[i].code < HEALTH_FAULT_COUNT) {
                faults |= HEALTH_BIT(now.counters[i].code);
            }
        }
        if (!time_sync_is_current(TIME_SYNC_TIMEOUT_MS)) {
            faults |= HEALTH_BIT(HEALTH_NO_TIME_SYNC);
        }
        size_t working = 0;
        for (size_t i = 0; i < now.sensor_count; i++) {
            uint32_t good = now.sensors[i].good_reads - previous.sensors[i].good_reads;
            uint32_t failed = now.sensors[i].failed_reads - previous.sensors[i].failed_reads;
            if (now.sensors[i].init_error != ESP_OK) {
                faults |= HEALTH_BIT(HEALTH_SENSOR_INIT);
            } else if (failed && !good) {
                faults |= HEALTH_BIT(HEALTH_SENSOR_DEAD);
            } else {
                if (failed) {
                    faults |= HEALTH_BIT(HEALTH_SENSOR_READ);
                }
                working++;
            }
        }
        previous = now;

        bool recording = sampler_is_active();
        node_status_frame_t status = {
            .faults = faults,
            .health = health_for(faults, now.sensor_count > 0 && working == 0),
            .mode = recording ? NODE_MODE_RECORDING : NODE_MODE_IDLE,
        };
        bool status_sent = can_node_send(CAN_ID_NODE_STATUS_BASE + NODE_ID, (const uint8_t *)&status,
                                         NODE_HEALTH_PAYLOAD) == ESP_OK;
        send_diagnostics();
        if (status_sent && recording) {
            report_events(&now, &reported, init_reported);
        }
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
    return xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
