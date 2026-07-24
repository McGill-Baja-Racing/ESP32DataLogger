#include <limits.h>
#include <stdint.h>

#include "sensor.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "protocol/app_protocol.h"

/*
 * Bearing encoder configuration
 * -----------------------------
 * Update this block when changing the bearing model, wiring, response time,
 * or direction convention. The current values are for SKF
 * BMB-6202/032S2/UB108A.
 */
#define ENCODER_MODEL                       "SKF BMB-6202/032S2/UB108A"
#define ENCODER_GPIO_A                      6
#define ENCODER_GPIO_B                      7
#define ENCODER_PULSES_PER_REVOLUTION       32
#define ENCODER_QUADRATURE_EDGES_PER_PULSE  4
#define ENCODER_SAMPLE_PERIOD_US            20000
#define ENCODER_RPM_WINDOW_US               100000
#define ENCODER_STOP_TIMEOUT_US             100000
#define ENCODER_DIRECTION_SIGN              1
#define ENCODER_ENABLE_INTERNAL_PULLUPS     1

#define ENCODER_COUNTS_PER_REVOLUTION \
    (ENCODER_PULSES_PER_REVOLUTION * ENCODER_QUADRATURE_EDGES_PER_PULSE)
#define ENCODER_WINDOW_SAMPLES \
    (ENCODER_RPM_WINDOW_US / ENCODER_SAMPLE_PERIOD_US)

_Static_assert(ENCODER_PULSES_PER_REVOLUTION > 0,
               "Encoder pulses per revolution must be positive");
_Static_assert(ENCODER_RPM_WINDOW_US >= ENCODER_SAMPLE_PERIOD_US,
               "RPM window must contain at least one sample");
_Static_assert(ENCODER_RPM_WINDOW_US % ENCODER_SAMPLE_PERIOD_US == 0,
               "RPM window must be an exact number of sample periods");
_Static_assert(ENCODER_DIRECTION_SIGN == 1 || ENCODER_DIRECTION_SIGN == -1,
               "Encoder direction sign must be 1 or -1");

static const int8_t transitions[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static int8_t encoder_transition(uint8_t previous, uint8_t current)
{
    return ENCODER_DIRECTION_SIGN *
           transitions[((previous & 0x3u) << 2) | (current & 0x3u)];
}

static int32_t calculate_rpm(int64_t counts, int64_t elapsed_us)
{
    if (elapsed_us <= 0 || counts == 0) {
        return 0;
    }

    int64_t numerator = counts * 60000000LL;
    int64_t denominator =
        (int64_t)ENCODER_COUNTS_PER_REVOLUTION * elapsed_us;
    int64_t rounded = numerator > 0
        ? (numerator + denominator / 2) / denominator
        : (numerator - denominator / 2) / denominator;

    if (rounded > INT32_MAX) {
        return INT32_MAX;
    }
    if (rounded < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)rounded;
}

static int encoder_is_stopped(int64_t last_edge_us, int64_t now_us)
{
    return last_edge_us <= 0 ||
           now_us - last_edge_us >= ENCODER_STOP_TIMEOUT_US;
}

typedef struct {
    volatile int32_t pending_count;
    volatile int64_t last_valid_edge_us;
    uint8_t last_state;
    portMUX_TYPE lock;
    int32_t count_window[ENCODER_WINDOW_SAMPLES];
    int64_t time_window_us[ENCODER_WINDOW_SAMPLES];
    uint8_t window_index;
    uint8_t window_size;
    int64_t last_read_us;
} encoder_context_t;

static encoder_context_t encoder = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint8_t read_state(void)
{
    return (gpio_get_level(ENCODER_GPIO_A) ? 2u : 0u) |
           (gpio_get_level(ENCODER_GPIO_B) ? 1u : 0u);
}

static void IRAM_ATTR encoder_isr(void *argument)
{
    encoder_context_t *context = argument;
    uint8_t state = read_state();
    portENTER_CRITICAL_ISR(&context->lock);
    int8_t transition =
        encoder_transition(context->last_state, state);
    if (transition != 0) {
        context->pending_count += transition;
        context->last_valid_edge_us = esp_timer_get_time();
    }
    context->last_state = state;
    portEXIT_CRITICAL_ISR(&context->lock);
}

static esp_err_t init_encoder(sensor_t *sensor)
{
    encoder_context_t *context = sensor->context;
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << ENCODER_GPIO_A) |
                        (1ULL << ENCODER_GPIO_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = ENCODER_ENABLE_INTERNAL_PULLUPS
            ? GPIO_PULLUP_ENABLE
            : GPIO_PULLUP_DISABLE,
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
    error = gpio_isr_handler_add(ENCODER_GPIO_A, encoder_isr, context);
    if (error != ESP_OK) {
        return error;
    }
    return gpio_isr_handler_add(ENCODER_GPIO_B, encoder_isr, context);
}

static void start_encoder(sensor_t *sensor)
{
    encoder_context_t *context = sensor->context;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&context->lock);
    context->pending_count = 0;
    context->last_valid_edge_us = 0;
    portEXIT_CRITICAL(&context->lock);

    for (uint8_t i = 0; i < ENCODER_WINDOW_SAMPLES; i++) {
        context->count_window[i] = 0;
        context->time_window_us[i] = 0;
    }
    context->window_index = 0;
    context->window_size = 0;
    context->last_read_us = now_us;
}

static int32_t read_rpm(sensor_t *sensor)
{
    encoder_context_t *context = sensor->context;
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - context->last_read_us;
    if (elapsed_us <= 0) {
        return 0;
    }
    context->last_read_us = now_us;

    portENTER_CRITICAL(&context->lock);
    int32_t count = context->pending_count;
    context->pending_count = 0;
    int64_t last_valid_edge_us = context->last_valid_edge_us;
    portEXIT_CRITICAL(&context->lock);

    context->count_window[context->window_index] = count;
    context->time_window_us[context->window_index] = elapsed_us;
    context->window_index =
        (uint8_t)((context->window_index + 1) % ENCODER_WINDOW_SAMPLES);
    if (context->window_size < ENCODER_WINDOW_SAMPLES) {
        context->window_size++;
    }

    if (encoder_is_stopped(last_valid_edge_us, now_us)) {
        return 0;
    }

    int64_t window_count = 0;
    int64_t window_elapsed_us = 0;
    for (uint8_t i = 0; i < context->window_size; i++) {
        window_count += context->count_window[i];
        window_elapsed_us += context->time_window_us[i];
    }
    return calculate_rpm(window_count, window_elapsed_us);
}

sensor_t bearing_encoder_sensor = {
    .name = "bearing_encoder",
    .can_id = CAN_ID_BEARING_ENCODER,
    .period_us = ENCODER_SAMPLE_PERIOD_US,
    .init = init_encoder,
    .start = start_encoder,
    .read = read_rpm,
    .context = &encoder,
};
