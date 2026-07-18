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
#include "time/time_sync.h"

/*
 * Generic sampling pipeline
 * -------------------------
 * sample_task schedules every descriptor returned by sensor_registry(). The
 * separate send_task drains the queue so a temporarily busy CAN controller
 * does not directly delay sensor reads.
 */

#define SAMPLE_QUEUE_LENGTH 64

typedef struct {
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
    xQueueReset(sample_queue);
    arm_schedule();
    active = true;
}

void sampler_stop(void)
{
    active = false;
    xQueueReset(sample_queue);
}

void sampler_discard_pending(void)
{
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

static void sample_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t next_due = now + 1000000;
        for (size_t i = 0; i < sensor_count; i++) {
            sensor_t *sensor = &sensors[i];
            if (now >= sensor->next_sample_us) {
                /* Hardware-specific work is confined to the sensor callback. */
                sample_t sample = {
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
        if (sleep_us >= 2000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
        } else {
            taskYIELD();
        }
    }
}

static void send_task(void *argument)
{
    (void)argument;
    sample_t sample;
    while (true) {
        xQueueReceive(sample_queue, &sample, portMAX_DELAY);
        if (!active) {
            continue;
        }
        uint8_t payload[8];
        uint64_t packed = ((uint64_t)(uint32_t)sample.timestamp_ms << 32) |
                          (uint32_t)sample.value;
        memcpy(payload, &packed, sizeof(payload));
        esp_err_t error = can_node_send(sample.can_id, payload, sizeof(payload));
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Sensor 0x%03" PRIX32 " TX failed: %s",
                     sample.can_id, esp_err_to_name(error));
        }
    }
}

esp_err_t sampler_init(void)
{
    sensors = sensor_registry(&sensor_count);
#if NODE_FIXED_BRAKE_CONFIG || NODE_FIXED_ENGINE_CONFIG
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
