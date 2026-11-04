#include "health/node_health.h"

#include <inttypes.h>
#include <string.h>

#include "can/can_node.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sampler/sampler.h"
#include "sensors/sensor.h"
#include "time/time_sync.h"

/*
 * A count is marked reported once the CAN driver holds its event, so counts
 * made while idle are sent later, stamped with when they last happened. An
 * event the driver then drops shows up at the master as a gap in the event
 * sequence. Capping counts at HEALTH_EVENT_COUNT_MAX means a fault storm while
 * idle leaves no backlog. Occurrence times are kept on the local clock because
 * counting runs in interrupts; they are converted when sent.
 */

#define TIME_SYNC_TIMEOUT_MS 1000

/* One report sends at most one event per sensor and counter. With 64 or more,
 * a whole lost report could wrap the sequence unseen. */
_Static_assert(MAX_SENSORS + HEALTH_COUNTER_COUNT < HEALTH_EVENT_SEQUENCE_MASK + 1,
               "a report must fit the event sequence");

typedef struct {
    uint32_t can_id;
    unsigned slot;
    esp_err_t init_error;
    uint32_t init_ms;
    uint32_t good_reads;
    uint32_t failed_reads;
    esp_err_t last_error;
    uint32_t last_failure_ms;
} sensor_health_t;

/* What one report works from. The sensors are copied whole under the lock so
 * a status and its read events agree. */
typedef struct {
    health_state_t counts;
    sensor_health_t sensors[MAX_SENSORS];
    size_t sensor_count;
} node_snapshot_t;

static const char *TAG = "Health";
static sensor_health_t sensor_health[MAX_SENSORS];
static size_t sensor_health_count;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t resets;
static uint8_t reset_reason;
static uint8_t event_sequence;

/**
 * @brief Get this node's own time in ms, not the master's.
 *
 * @return Milliseconds since boot.
 */
static uint32_t local_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void node_health_sensor_init(size_t index, uint32_t can_id, esp_err_t error)
{
    unsigned slot = protocol_sensor_slot(NODE_ID, can_id);
    if (slot >= NODE_SENSOR_SLOTS) {
        ESP_LOGE(TAG, "Sensor 0x%03" PRIX32 " has no status slot: not a sample ID of node %d,"
                 " or past the %d a status holds", can_id, NODE_ID, NODE_SENSOR_SLOTS);
    }
    uint32_t now = local_ms();
    portENTER_CRITICAL_SAFE(&lock);
    sensor_health[index].can_id = can_id;
    sensor_health[index].slot = slot;
    sensor_health[index].init_error = error;
    sensor_health[index].init_ms = now;
    if (index >= sensor_health_count) {
        sensor_health_count = index + 1;
    }
    portEXIT_CRITICAL_SAFE(&lock);
}

void node_health_sensor_read(size_t index, esp_err_t error)
{
    uint32_t now = local_ms();
    sensor_health_t *sensor = &sensor_health[index];
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

/**
 * @brief Judge a sensor from its reads in the last status period.
 *
 * @param init_failed true if the sensor never started.
 * @param good Reads that worked this period.
 * @param failed Reads that failed this period.
 * @return INIT_FAILED, OK, NO_READINGS (under 10 % worked) or SOME_FAILED.
 */
static sensor_state_t sensor_state(bool init_failed, uint32_t good, uint32_t failed)
{
    if (init_failed) {
        return SENSOR_STATE_INIT_FAILED;
    }
    if (failed == 0) {
        return SENSOR_STATE_OK;
    }
    if (good * 10 < good + failed) {
        return SENSOR_STATE_NO_READINGS;
    }
    return SENSOR_STATE_SOME_FAILED;
}

/**
 * @brief Send one event frame.
 *
 * Sends only while recording with the master's beacons arriving.
 *
 * @param code Event code.
 * @param subject Sent as one byte.
 * @param count Occurrences since the last report.
 * @param error Last error, or ESP_OK.
 * @param when_local_ms Local time of the last occurrence.
 * @return false if the frame was not sent.
 */
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
    health_event_frame_t event = node_health_pack_event(code, subject, event_sequence, count, error,
                                                        when_ms < 0 ? 0 : (uint32_t)when_ms);
    if (can_node_send(CAN_ID_NODE_EVENT_BASE + NODE_ID, (const uint8_t *)&event, sizeof(event)) != ESP_OK) {
        return false;
    }
    event_sequence++;
    return true;
}

/**
 * @brief Send an event if a counter grew since its last report.
 *
 * @param now Current counts.
 * @param reported Counts already reported, updated when sent.
 * @param i Counter index.
 * @return false if the event could not be sent.
 */
static bool report_counter(const health_state_t *now, health_state_t *reported, size_t i)
{
    const health_counter_t *counter = &now->counters[i];
    uint32_t count = counter->count - reported->counters[i].count;
    if (count && !send_event(counter->code, counter->subject, count, counter->error,
                             counter->last_ms)) {
        return false;
    }
    reported->counters[i].count = counter->count;
    return true;
}

/**
 * @brief Send every event not yet reported.
 *
 * Stops at the first unsent event, so a failing bus does not delay samples.
 *
 * @param now Current state.
 * @param reported Counts already reported, updated as events are sent.
 * @param init_reported Per sensor, true once its init failure was sent.
 */
static void report_events(const node_snapshot_t *now, node_snapshot_t *reported,
                          bool *init_reported)
{
    /* The master restarts its sequence check at the boot event. */
    if (!report_counter(&now->counts, &reported->counts, HEALTH_BOOT)) {
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
        uint32_t failed = sensor->failed_reads - reported->sensors[i].failed_reads;
        if (failed && !send_event(HEALTH_SENSOR_READ, sensor->can_id, failed,
                                  sensor->last_error, sensor->last_failure_ms)) {
            return;
        }
        reported->sensors[i].failed_reads = sensor->failed_reads;
    }
    for (size_t i = 0; i < HEALTH_COUNTER_COUNT; i++) {
        if (!report_counter(&now->counts, &reported->counts, i)) {
            return;
        }
    }
}

/**
 * @brief Send the diagnostics frame.
 */
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

/**
 * @brief Task that reports the node's health. See node_health_init().
 */
static void health_task(void *argument)
{
    (void)argument;
    /* Static to keep three copies of the state off the task stack. */
    static node_snapshot_t now, previous, reported;
    static bool init_reported[MAX_SENSORS];
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(NODE_STATUS_PERIOD_MS));
        node_health_copy_counts(&now.counts);
        portENTER_CRITICAL(&lock);
        memcpy(now.sensors, sensor_health, sizeof(sensor_health));
        now.sensor_count = sensor_health_count;
        portEXIT_CRITICAL(&lock);

        /* Status covers only the last period; events carry everything else. */
        uint32_t faults = node_health_get_faults(&now.counts, &previous.counts);
        if (!time_sync_is_current(TIME_SYNC_TIMEOUT_MS)) {
            faults |= HEALTH_BIT(HEALTH_NO_TIME_SYNC);
        }
        bool recording = sampler_is_active();
        size_t working = 0;
        uint32_t sensors = 0;
        sensor_state_t worst = SENSOR_STATE_OK;
        for (size_t i = 0; i < now.sensor_count; i++) {
            uint32_t good = now.sensors[i].good_reads - previous.sensors[i].good_reads;
            uint32_t failed = now.sensors[i].failed_reads - previous.sensors[i].failed_reads;
            sensor_state_t sensor = sensor_state(now.sensors[i].init_error != ESP_OK, good, failed);
            if (sensor > worst) {
                worst = sensor;
            }
            if (sensor <= SENSOR_STATE_SOME_FAILED) {
                working++;
            }
            if (now.sensors[i].slot < NODE_SENSOR_SLOTS) {
                sensors |= (uint32_t)sensor << (2 * now.sensors[i].slot);
            }
        }
        previous = now;

        node_status_frame_t status = {
            .faults = faults,
            .health = health_for(faults, worst, now.sensor_count > 0 && working == 0),
            .mode = recording ? NODE_MODE_RECORDING : NODE_MODE_IDLE,
            .sensors = sensors,
        };
        bool status_sent = can_node_send(CAN_ID_NODE_STATUS_BASE + NODE_ID, (const uint8_t *)&status,
                                         NODE_HEALTH_PAYLOAD) == ESP_OK;
        send_diagnostics();
        if (status_sent) {
            report_events(&now, &reported, init_reported);
        }
    }
}

esp_err_t node_health_init(void)
{
    node_health_count_boot(&resets, &reset_reason);
    return xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}
