#include "sampler.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can/can_node.h"
#include "health/node_health.h"
#include "sensors/adc_input.h"
#include "sensors/sensor.h"
#include "sensors/engine_rpm.h"
#include "time/time_sync.h"

#ifndef SENSOR_SERIAL_TEST
#define SENSOR_SERIAL_TEST 0
#endif

/*
 * Generic sampling pipeline
 * -------------------------
 * sample_task schedules every descriptor returned by sensor_registry(). The
 * separate send_task drains the queue so a temporarily busy CAN controller
 * does not directly delay sensor reads.
 */

#define SAMPLE_QUEUE_LENGTH 64
#define SERIAL_LOG_SLOT_COUNT 16

typedef struct {
    const char *sensor_name;
    uint32_t can_id;
    int32_t timestamp_ms;
    int32_t value;
} sample_t;

static const char *TAG = "Sampler";
static QueueHandle_t sample_queue;
static sensor_t *sensors;
static size_t sensor_count;
/* Sensors whose init failed stay registered for health reports but are never sampled. */
static bool sensor_ready[MAX_SENSORS];
static volatile bool active;

static void arm_schedule(void)
{
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < sensor_count; i++) {
        sensors[i].next_sample_us = now;
    }
}

/**
 * @brief Initialize one sensor and record whether it is ready.
 *
 * Sets up the shared ADC unit on brake/ADC nodes, then runs the sensor's init
 * callback. Stores the result in sensor_ready[i], reports it to node health,
 * and logs it. Runs at boot and again on each start until it succeeds; a
 * failed sensor is only skipped. adc_input_init() is idempotent, so calling it
 * per sensor is safe.
 *
 * @param i Index into sensors[].
 */
static void init_sensor(size_t i)
{
    esp_err_t error = ESP_OK;
#if NODE_FIXED_BRAKE_CONFIG || NODE_FIXED_ADC_CONFIG
    error = adc_input_init();
#endif
    if (error == ESP_OK && sensors[i].init) {
        error = sensors[i].init(&sensors[i]);
    }
    sensor_ready[i] = (error == ESP_OK);
    node_health_sensor_init(i, sensors[i].can_id, error);
    ESP_LOGI(TAG, "%s: CAN=0x%03" PRIX32 " period=%" PRIu32 "us init=%s",
             sensors[i].name, sensors[i].can_id, sensors[i].period_us,
             esp_err_to_name(error));
}

void sampler_start(void)
{
    if (active) {
        return;
    }
    xQueueReset(sample_queue);
    for (size_t i = 0; i < sensor_count; i++) {
        /* Retry failed inits so a glitch at boot does not last the whole session. */
        if (!sensor_ready[i]) {
            init_sensor(i);
        }
        if (sensor_ready[i] && sensors[i].start) {
            sensors[i].start(&sensors[i]);
        }
    }
    arm_schedule();
    active = true;
}

void sampler_stop(void)
{
    if (!active) {
        return;
    }
    active = false;
    xQueueReset(sample_queue);
}

void sampler_discard_pending(void)
{
#if NODE_FIXED_ENGINE_CONFIG
    engine_rpm_discard_pending();
#endif
    sample_t discarded;
    while (xQueueReceive(sample_queue, &discarded, 0) == pdTRUE) {
        node_health_count(HEALTH_SAMPLES_DROPPED, 1, discarded.can_id, ESP_OK);
    }
}

bool sampler_is_active(void)
{
    return active;
}

/**
 * @brief Queue a sample for send_task without blocking.
 *
 * If the queue is full, drops the oldest sample, counts it as dropped in node
 * health, and queues this one in its place.
 *
 * @param sample Copied into the queue.
 */
static void enqueue_latest(sample_t *sample)
{
    if (xQueueSend(sample_queue, sample, 0) == pdTRUE) {
        return;
    }
    /* Prefer current measurements when the queue cannot keep up. */
    sample_t discarded;
    if (xQueueReceive(sample_queue, &discarded, 0) == pdTRUE) {
        node_health_count(HEALTH_SAMPLES_DROPPED, 1, discarded.can_id, ESP_OK);
    }
    (void)xQueueSend(sample_queue, sample, 0);
}

#if NODE_FIXED_ENGINE_CONFIG
/**
 * @brief Turn one engine event into spark and RPM samples and queue them.
 *
 * A spark queues a spark sample (value 1), and an RPM sample when RPM > 0. A
 * stop event queues only its RPM 0 sample. Both samples are queued or neither:
 * if the queue lacks room, the event is dropped and each sample counts as
 * dropped in node health. In serial test builds only the RPM sample is queued,
 * through enqueue_latest(). A negative timestamp counts as a bad timestamp.
 *
 * @param event Event from engine_rpm_next_event().
 */
static void enqueue_engine_event(const engine_event_t *event)
{
    sample_t rpm = {
        .sensor_name = "engine_rpm",
        .can_id = CAN_ID_ENGINE_RPM,
        .timestamp_ms = time_sync_timestamp_ms_at(event->timestamp_us),
        .value = event->engine_rpm,
    };
    if (rpm.timestamp_ms < 0) {
        node_health_count(HEALTH_BAD_TIMESTAMP, 1, CAN_ID_ENGINE_RPM, ESP_OK);
    }
    bool send_rpm = !event->spark || event->engine_rpm > 0;
#if SENSOR_SERIAL_TEST
    /* Bench output uses the same value and throttling as every sensor. */
    if (send_rpm) {
        enqueue_latest(&rpm);
    }
#else
    UBaseType_t count = (event->spark ? 1 : 0) + (send_rpm ? 1 : 0);
    /* This task is the only producer. Reserve room for the whole event so
     * queue pressure cannot split a spark from its RPM measurement. */
    if (uxQueueSpacesAvailable(sample_queue) < count) {
        ESP_LOGW(TAG, "Spark/RPM output queue full; event lost");
        if (event->spark) {
            node_health_count(HEALTH_SAMPLES_DROPPED, 1,
                              CAN_ID_ENGINE_SPARK, ESP_OK);
        }
        if (send_rpm) {
            node_health_count(HEALTH_SAMPLES_DROPPED, 1,
                              CAN_ID_ENGINE_RPM, ESP_OK);
        }
        return;
    }
    if (event->spark) {
        sample_t spark = {
            .sensor_name = "engine_spark",
            .can_id = CAN_ID_ENGINE_SPARK,
            .timestamp_ms = rpm.timestamp_ms,
            .value = 1,
        };
        (void)xQueueSend(sample_queue, &spark, 0);
    }
    if (send_rpm) {
        (void)xQueueSend(sample_queue, &rpm, 0);
    }
#endif
}
#endif

/**
 * @brief Task that reads sensors on schedule and queues samples for send_task.
 *
 * Polls every 20 ms while inactive. On the engine node it waits for engine
 * events instead and reports sparks lost by the capture queue. Otherwise it
 * reads each ready sensor that is due and queues successful reads; read errors
 * go to node health. A late sensor skips the periods it missed rather than
 * catching up, and they count as missed. Then it sleeps until the next sensor
 * is due: at least one tick, at most 1 s.
 */
static void sample_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

#if NODE_FIXED_ENGINE_CONFIG
        if (!sensor_ready[0]) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        engine_event_t event;
        if (engine_rpm_next_event(&event) && active) {
            enqueue_engine_event(&event);
        }
        uint32_t dropped = engine_rpm_dropped_events();
        if (dropped) {
            ESP_LOGW(TAG, "Spark capture overflow: %" PRIu32 " events lost", dropped);
            node_health_count(HEALTH_SAMPLES_DROPPED, dropped, CAN_ID_ENGINE_SPARK, ESP_OK);
        }
#else
        int64_t now = esp_timer_get_time();
        int64_t next_due = now + 1000000;
        for (size_t i = 0; i < sensor_count; i++) {
            sensor_t *sensor = &sensors[i];
            if (!sensor_ready[i]) {
                continue;
            }
            if (now >= sensor->next_sample_us) {
                /* Hardware-specific work is confined to the sensor callback. */
                sample_t sample = {
                    .sensor_name = sensor->name,
                    .can_id = sensor->can_id,
                    .timestamp_ms = time_sync_timestamp_ms(),
                };
                esp_err_t error = sensor->read(sensor, &sample.value);
                node_health_sensor_read(i, error);
                if (sample.timestamp_ms < 0) {
                    node_health_count(HEALTH_BAD_TIMESTAMP, 1, sensor->can_id, ESP_OK);
                }
                uint32_t periods = 0;
                while (sensor->next_sample_us <= now) {
                    sensor->next_sample_us += sensor->period_us;
                    periods++;
                }
                if (periods > 1) {
                    node_health_count(HEALTH_SAMPLES_MISSED, periods - 1, sensor->can_id, ESP_OK);
                }
                if (error == ESP_OK) {
                    enqueue_latest(&sample);
                }
            }
            if (sensor->next_sample_us < next_due) {
                next_due = sensor->next_sample_us;
            }
        }

        int64_t sleep_us = next_due - esp_timer_get_time();
        /* Sub-tick sleeps must still block. At the configured 100 Hz tick
         * rate, pdMS_TO_TICKS(2..9) is zero and would starve the idle task. */
        TickType_t sleep_ticks = sleep_us > 0
            ? pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)) : 0;
        vTaskDelay(sleep_ticks > 0 ? sleep_ticks : 1);
#endif
    }
}

/**
 * @brief Task that sends queued samples over CAN.
 *
 * Blocks until a sample arrives and discards it if sampling has stopped. Each
 * sample becomes one 8-byte frame on its sensor's CAN ID: value in bytes 0-3,
 * timestamp_ms in bytes 4-7, little-endian. A failed send is logged and the
 * sample is lost. In serial test builds it prints samples instead, at most one
 * per sensor every 500 ms.
 */
static void send_task(void *argument)
{
    (void)argument;
    sample_t sample;
#if SENSOR_SERIAL_TEST
    typedef struct {
        const char *sensor_name;
        int64_t next_log_us;
    } serial_log_slot_t;
    serial_log_slot_t log_slots[SERIAL_LOG_SLOT_COUNT] = {0};
#endif
    while (true) {
        xQueueReceive(sample_queue, &sample, portMAX_DELAY);
        if (!active) {
            continue;
        }
#if SENSOR_SERIAL_TEST
        /* Throttle each sensor independently so multi-sensor bench
         * configurations remain readable without hiding any sensor. */
        size_t slot = 0;
        while (slot < SERIAL_LOG_SLOT_COUNT &&
               log_slots[slot].sensor_name != NULL &&
               log_slots[slot].sensor_name != sample.sensor_name) {
            slot++;
        }
        if (slot == SERIAL_LOG_SLOT_COUNT) {
            ESP_LOGW(TAG, "No serial log slot available for %s",
                     sample.sensor_name);
            continue;
        }
        if (log_slots[slot].sensor_name == NULL) {
            log_slots[slot].sensor_name = sample.sensor_name;
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us >= log_slots[slot].next_log_us) {
            ESP_LOGI("SensorTest",
                     "%s: CAN=0x%03" PRIX32 " time=%" PRId32
                     "ms value=%" PRId32,
                     sample.sensor_name, sample.can_id,
                     sample.timestamp_ms, sample.value);
            log_slots[slot].next_log_us = now_us + 500000;
        }
#else
        uint8_t payload[8];
        memcpy(&payload[0], &sample.value, 4);
        memcpy(&payload[4], &sample.timestamp_ms, 4);
        esp_err_t error = can_node_send(sample.can_id, payload, sizeof(payload));
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Sensor 0x%03" PRIX32 " TX failed: %s",
                     sample.can_id, esp_err_to_name(error));
        }
#endif
    }
}

esp_err_t sampler_init(void)
{
    sensors = sensor_registry(&sensor_count);
    if (sensor_count > MAX_SENSORS) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < sensor_count; i++) {
        init_sensor(i);
    }

    sample_queue = xQueueCreate(SAMPLE_QUEUE_LENGTH, sizeof(sample_t));
    if (!sample_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(send_task, "sample_send", 4096, NULL, 7, NULL) != pdPASS ||
        xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
