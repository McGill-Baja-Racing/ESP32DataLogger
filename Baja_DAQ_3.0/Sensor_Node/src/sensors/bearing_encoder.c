#include "sensor.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "protocol/app_protocol.h"

#define GPIO_A                  6
#define GPIO_B                  7
#define COUNTS_PER_REVOLUTION   (32 * 4)
#define DEGREES_SCALE           10

typedef struct {
    volatile int32_t count;
    uint8_t last_state;
    portMUX_TYPE lock;
} encoder_context_t;

static encoder_context_t encoder = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static const int8_t transitions[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static uint8_t read_state(void)
{
    return (gpio_get_level(GPIO_A) ? 2u : 0u) |
           (gpio_get_level(GPIO_B) ? 1u : 0u);
}

static void IRAM_ATTR encoder_isr(void *argument)
{
    encoder_context_t *context = argument;
    uint8_t state = read_state();
    portENTER_CRITICAL_ISR(&context->lock);
    context->count += transitions[(context->last_state << 2) | state];
    context->last_state = state;
    portEXIT_CRITICAL_ISR(&context->lock);
}

static esp_err_t init_encoder(sensor_t *sensor)
{
    encoder_context_t *context = sensor->context;
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_A) | (1ULL << GPIO_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    context->last_state = read_state();
    error = gpio_isr_handler_add(GPIO_A, encoder_isr, context);
    if (error != ESP_OK) {
        return error;
    }
    return gpio_isr_handler_add(GPIO_B, encoder_isr, context);
}

static int32_t read_angle(sensor_t *sensor)
{
    encoder_context_t *context = sensor->context;
    portENTER_CRITICAL(&context->lock);
    int32_t count = context->count;
    portEXIT_CRITICAL(&context->lock);
    return (int32_t)(((int64_t)count * 360 * DEGREES_SCALE) /
                     COUNTS_PER_REVOLUTION);
}

sensor_t bearing_encoder_sensor = {
    .name = "bearing_encoder",
    .can_id = CAN_ID_BEARING_ENCODER,
    .period_us = 20000,
    .init = init_encoder,
    .read = read_angle,
    .context = &encoder,
};
