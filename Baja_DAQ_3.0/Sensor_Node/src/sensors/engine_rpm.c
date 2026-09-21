#include "sensor.h"
#include "engine_rpm.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol/app_protocol.h"

#define ENGINE_RPM_GPIO             GPIO_NUM_3
#define REVOLUTIONS_PER_SPARK       1U
#define MIN_ENGINE_RPM              1000U
#define MAX_ENGINE_RPM              6000U
#define ENGINE_STOP_TIMEOUT_US      100000
#define MIN_SPARK_INTERVAL_US \
    ((60000000U * REVOLUTIONS_PER_SPARK) / MAX_ENGINE_RPM)
#define MAX_SPARK_INTERVAL_US \
    ((60000000U * REVOLUTIONS_PER_SPARK) / MIN_ENGINE_RPM)


_Static_assert(MIN_ENGINE_RPM > 0U,
               "Minimum engine RPM must be positive");
_Static_assert(MIN_ENGINE_RPM < MAX_ENGINE_RPM,
               "Engine RPM range is invalid");

typedef struct {
    int64_t timestamp_us;
    int64_t elapsed_us;
    bool valid;
} spark_capture_t;

typedef struct {
    int64_t last_spark_us;
    int64_t last_zero_us;
    uint32_t dropped;
    QueueHandle_t events;
    portMUX_TYPE lock;
} engine_rpm_context_t;

static engine_rpm_context_t engine = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void IRAM_ATTR engine_rpm_isr(void *argument)
{
    engine_rpm_context_t *context = argument;
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&context->lock);
    int64_t now_us = esp_timer_get_time();
    spark_capture_t capture = {
        .timestamp_us = now_us,
        .elapsed_us = now_us - context->last_spark_us,
    };
    capture.valid = context->last_spark_us != 0 &&
        capture.elapsed_us >= MIN_SPARK_INTERVAL_US &&
        capture.elapsed_us <= MAX_SPARK_INTERVAL_US;
    /* Measure consecutive detected edges. Advancing even after rejection
     * prevents an overspeed pulse train from aliasing into an accepted RPM. */
    context->last_spark_us = now_us;
    if (xQueueSendFromISR(context->events, &capture, &wake) != pdTRUE) {
        context->dropped++;
    }
    portEXIT_CRITICAL_ISR(&context->lock);
    if (wake) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_engine_rpm(sensor_t *sensor)
{
    engine_rpm_context_t *context = sensor->context;

    context->events = xQueueCreate(256, sizeof(spark_capture_t));
    if (!context->events) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = gpio_reset_pin(ENGINE_RPM_GPIO);
    if (error != ESP_OK) {
        return error;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << ENGINE_RPM_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }

    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = gpio_isr_handler_add(ENGINE_RPM_GPIO, engine_rpm_isr, context);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_intr_enable(ENGINE_RPM_GPIO);
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI("EngineRPM",
             "Digital rising-edge input enabled on GPIO%d (initial level=%d)",
             ENGINE_RPM_GPIO, gpio_get_level(ENGINE_RPM_GPIO));
    return ESP_OK;
}

void engine_rpm_discard_pending(void)
{
    engine_rpm_context_t *context = &engine;
    portENTER_CRITICAL(&context->lock);
    xQueueReset(context->events);
    context->last_spark_us = 0;
    context->last_zero_us = esp_timer_get_time();
    context->dropped = 0;
    portEXIT_CRITICAL(&context->lock);
}

static void start_engine_rpm(sensor_t *sensor)
{
    (void)sensor;
    engine_rpm_discard_pending();
}

bool engine_rpm_next_event(engine_event_t *event)
{
    spark_capture_t capture;
    if (xQueueReceive(engine.events, &capture, 1) != pdTRUE) {
        portENTER_CRITICAL(&engine.lock);
        int64_t now = esp_timer_get_time();
        bool stopped = now - engine.last_spark_us >= ENGINE_STOP_TIMEOUT_US &&
                       now - engine.last_zero_us >= ENGINE_STOP_TIMEOUT_US;
        if (stopped) {
            engine.last_zero_us = now;
        }
        portEXIT_CRITICAL(&engine.lock);
        if (!stopped) return false;
        *event = (engine_event_t){.timestamp_us = now};
        return true;
    }
    event->spark = true;
    event->timestamp_us = capture.timestamp_us;
    event->engine_rpm = capture.valid
        ? (int32_t)(60000000LL * REVOLUTIONS_PER_SPARK / capture.elapsed_us) : 0;
    return true;
}

uint32_t engine_rpm_dropped_events(void)
{
    portENTER_CRITICAL(&engine.lock);
    uint32_t dropped = engine.dropped;
    engine.dropped = 0;
    portEXIT_CRITICAL(&engine.lock);
    return dropped;
}

sensor_t engine_rpm_sensor = {
    .name = "engine_rpm",
    .can_id = CAN_ID_ENGINE_RPM,
    .period_us = 0, /* Spark driven, no periodic reads. */
    .init = init_engine_rpm,
    .start = start_engine_rpm,
    .context = &engine,
};
