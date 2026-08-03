#include "sensor.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "protocol/app_protocol.h"
#include "diagnostics/diagnostics.h"

#define ENGINE_RPM_GPIO             GPIO_NUM_3
#define REVOLUTIONS_PER_SPARK       1U
#define MAX_ENGINE_RPM              4000U
#define MIN_SPARK_INTERVAL_US       3000U
#define ENGINE_STOP_TIMEOUT_US      500000LL
#define ENGINE_REJECT_THRESHOLD     5U
#define ENGINE_REJECT_CLEAR_US      2000000LL

/* At 4000 RPM, valid sparks are at least 15 ms apart. The shorter 3 ms
 * rejection window ignores ringing around a single pulse without masking the
 * next legitimate spark. */
_Static_assert(MIN_SPARK_INTERVAL_US <
                   (60000000U / MAX_ENGINE_RPM),
               "Spark rejection window masks valid engine pulses");

typedef struct {
    volatile int64_t last_spark_us;
    volatile uint32_t spark_period_us;
    volatile uint32_t rejected_pulses;
    volatile int64_t last_rejected_us;
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
        } else {
            context->rejected_pulses++;
            context->last_rejected_us = now_us;
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
    uint32_t rejected = context->rejected_pulses;
    context->rejected_pulses = 0;
    int64_t last_rejected = context->last_rejected_us;
    portEXIT_CRITICAL(&context->lock);

    int64_t now_us = esp_timer_get_time();
    static uint32_t reject_window_count;static int64_t reject_window_start;
    if(!reject_window_start)reject_window_start=now_us;reject_window_count+=rejected;
    if(now_us-reject_window_start>=1000000){if(reject_window_count>=ENGINE_REJECT_THRESHOLD)diagnostics_report(DIAG_ENGINE_REJECTED_PULSE,true,DIAG_WARNING,true);reject_window_count=0;reject_window_start=now_us;}
    if(last_rejected&&now_us-last_rejected>=ENGINE_REJECT_CLEAR_US)diagnostics_report(DIAG_ENGINE_REJECTED_PULSE,false,DIAG_WARNING,true);
    if (last_spark_us == 0 || period_us == 0 ||
        now_us - last_spark_us > ENGINE_STOP_TIMEOUT_US) {
        return 0;
    }

    int32_t rpm=(int32_t)((60000000ULL * REVOLUTIONS_PER_SPARK) / period_us);
    static uint8_t bad,good;
    if(rpm>MAX_ENGINE_RPM){good=0;if(++bad>=3)diagnostics_report(DIAG_ENGINE_RPM,true,DIAG_WARNING,true);}else{bad=0;if(++good>=5)diagnostics_report(DIAG_ENGINE_RPM,false,DIAG_WARNING,true);}
    return rpm;
}

sensor_t engine_rpm_sensor = {
    .name = "engine_rpm",
    .can_id = CAN_ID_ENGINE_RPM,
    .period_us = 20000, /* 50 Hz reporting rate */
    .init = init_engine_rpm,
    .read = read_engine_rpm,
    .context = &engine,
};
