#include "sampler.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can/can_node.h"
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
static volatile bool active;

static void arm_schedule(void)
{
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < sensor_count; i++) {
        sensors[i].next_sample_us = now;
    }
}

void sampler_start(void)
{
    if (active) {
        return;
    }
    xQueueReset(sample_queue);
    for (size_t i = 0; i < sensor_count; i++) {
        if (sensors[i].start) {
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
    xQueueReset(sample_queue);
}

bool sampler_is_active(void)
{
    return active;
}

static void enqueue_latest(sample_t *sample)
{
    if (xQueueSend(sample_queue, sample, 0) == pdTRUE) {
        return;
    }
    /* Prefer current measurements when the queue cannot keep up. */
    sample_t discarded;
    (void)xQueueReceive(sample_queue, &discarded, 0);
    (void)xQueueSend(sample_queue, sample, 0);
}

#if NODE_FIXED_ENGINE_CONFIG
static void enqueue_engine_event(const engine_event_t *event)
{
    sample_t sample = {
        .sensor_name = "engine_rpm",
        .can_id = CAN_ID_ENGINE_RPM,
        .timestamp_ms = time_sync_timestamp_ms_at(event->timestamp_us),
        .value = event->engine_rpm,
    };
    bool has_rpm = !event->spark || event->engine_rpm > 0;
#if SENSOR_SERIAL_TEST
    /* Bench output uses the same value and throttling as every sensor. */
    if (has_rpm) {
        enqueue_latest(&sample);
    }
#else
    UBaseType_t count = (event->spark ? 1 : 0) + (has_rpm ? 1 : 0);
    /* This task is the only producer. Reserve room for the whole event so
     * queue pressure cannot split a spark from its RPM measurement. */
    if (uxQueueSpacesAvailable(sample_queue) < count) {
        ESP_LOGW(TAG, "Spark/RPM output queue full; event lost");
        return;
    }
    if (event->spark) {
        sample_t spark = {
            .sensor_name = "engine_spark",
            .can_id = CAN_ID_ENGINE_SPARK,
            .timestamp_ms = sample.timestamp_ms,
            .value = 1,
        };
        (void)xQueueSend(sample_queue, &spark, 0);
    }
    if (has_rpm) {
        (void)xQueueSend(sample_queue, &sample, 0);
    }
#endif
}
#endif

static void sample_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

#if NODE_FIXED_ENGINE_CONFIG
        engine_event_t event;
        if (engine_rpm_next_event(&event) && active) {
            enqueue_engine_event(&event);
        }
        uint32_t dropped = engine_rpm_dropped_events();
        if (dropped) {
            ESP_LOGW(TAG, "Spark capture overflow: %" PRIu32 " events lost", dropped);
        }
        continue;
#endif
        int64_t now = esp_timer_get_time();
        int64_t next_due = now + 1000000;
        for (size_t i = 0; i < sensor_count; i++) {
            sensor_t *sensor = &sensors[i];
            if (now >= sensor->next_sample_us) {
                /* Hardware-specific work is confined to the sensor callback. */
                sample_t sample = {
                    .sensor_name = sensor->name,
                    .can_id = sensor->can_id,
                    .timestamp_ms = time_sync_timestamp_ms(),
                    .value = sensor->read(sensor),
                };
                while (sensor->next_sample_us <= now) {
                    sensor->next_sample_us += sensor->period_us;
                }
                enqueue_latest(&sample);
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
    }
}

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
        uint64_t packed = ((uint64_t)(uint32_t)sample.timestamp_ms << 32) |
                          (uint32_t)sample.value;
        memcpy(payload, &packed, sizeof(payload));
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
#if NODE_FIXED_BRAKE_CONFIG || NODE_FIXED_ADC_CONFIG
    esp_err_t error = adc_input_init();
    if (error != ESP_OK) {
        return error;
    }
#endif
    for (size_t i = 0; i < sensor_count; i++) {
        if (sensors[i].init) {
            esp_err_t error = sensors[i].init(&sensors[i]);
            if (error != ESP_OK) {
                return error;
            }
        }
        ESP_LOGI(TAG, "%s: CAN=0x%03" PRIX32 " period=%" PRIu32 "us",
                 sensors[i].name, sensors[i].can_id, sensors[i].period_us);
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
