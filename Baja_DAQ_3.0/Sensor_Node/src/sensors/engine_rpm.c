#include "sensor.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "protocol/app_protocol.h"

#define ENGINE_RPM_GPIO             GPIO_NUM_3
#define REVOLUTIONS_PER_SPARK       1U
#define MAX_ENGINE_RPM              4000U
#define MIN_SPARK_INTERVAL_US       3000U
#define ENGINE_STOP_TIMEOUT_US      500000LL

/* At 4000 RPM, valid sparks are at least 15 ms apart. The shorter 3 ms
 * rejection window ignores ringing around a single pulse without masking the
 * next legitimate spark. */
_Static_assert(MIN_SPARK_INTERVAL_US <
                   (60000000U / MAX_ENGINE_RPM),
               "Spark rejection window masks valid engine pulses");

typedef struct {
    volatile int64_t last_spark_us;
    volatile uint32_t spark_period_us;
    portMUX_TYPE lock;
} engine_rpm_context_t;

static engine_rpm_context_t engine = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void IRAM_ATTR engine_rpm_isr(void *argument)
{
    engine_rpm_context_t *context = argument;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&context->lock);
    if (context->last_spark_us == 0) {
        context->last_spark_us = now_us;
    } else {
        uint32_t elapsed_us =
            (uint32_t)(now_us - context->last_spark_us);
        if (elapsed_us >= MIN_SPARK_INTERVAL_US) {
            context->spark_period_us = elapsed_us;
            context->last_spark_us = now_us;
        }
    }
    portEXIT_CRITICAL_ISR(&context->lock);
}

static esp_err_t init_engine_rpm(sensor_t *sensor)
{
    engine_rpm_context_t *context = sensor->context;

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

static int32_t read_engine_rpm(sensor_t *sensor)
{
    engine_rpm_context_t *context = sensor->context;

    portENTER_CRITICAL(&context->lock);
    int64_t last_spark_us = context->last_spark_us;
    uint32_t period_us = context->spark_period_us;
    portEXIT_CRITICAL(&context->lock);

    int64_t now_us = esp_timer_get_time();
    if (last_spark_us == 0 || period_us == 0 ||
        now_us - last_spark_us > ENGINE_STOP_TIMEOUT_US) {
        return 0;
    }

    return (int32_t)((60000000ULL * REVOLUTIONS_PER_SPARK) / period_us);
}

sensor_t engine_rpm_sensor = {
    .name = "engine_rpm",
    .can_id = CAN_ID_ENGINE_RPM,
    .period_us = 20000, /* 50 Hz reporting rate */
    .init = init_engine_rpm,
    .read = read_engine_rpm,
    .context = &engine,
};
