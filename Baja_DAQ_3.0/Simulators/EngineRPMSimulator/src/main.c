#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OUTPUT_GPIO          GPIO_NUM_8
#define DEFAULT_RPM          1000U
#define MAX_RPM              4000U
#define PULSE_HIGH_US        200U
#define TIMER_RESOLUTION_HZ  1000000U

typedef struct {
    uint32_t rpm;
    uint32_t period_us;
    bool output_high;
    bool sweep_active;
    uint32_t sweep_generation;
    portMUX_TYPE lock;
} simulator_state_t;

static const char *TAG = "EngineRPMSim";
static gptimer_handle_t pulse_timer;
static simulator_state_t state = {
    .rpm = DEFAULT_RPM,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint32_t rpm_to_period_us(uint32_t rpm)
{
    return rpm ? (uint32_t)(60000000ULL / rpm) : 0;
}
static void set_manual_rpm(uint32_t rpm)
{
    portENTER_CRITICAL(&state.lock);
    state.rpm = rpm;
    state.period_us = rpm_to_period_us(rpm);
    state.output_high = false;
    state.sweep_active = false;
    state.sweep_generation++;
    portEXIT_CRITICAL(&state.lock);
    gpio_set_level(OUTPUT_GPIO, 0);
    ESP_LOGI(TAG, "Target set to %lu RPM", (unsigned long)rpm);
}

static void set_sweep_rpm(uint32_t rpm)
{
    portENTER_CRITICAL(&state.lock);
    state.rpm = rpm;
    state.period_us = rpm_to_period_us(rpm);
    state.output_high = false;
    portEXIT_CRITICAL(&state.lock);
    gpio_set_level(OUTPUT_GPIO, 0);
}

static bool IRAM_ATTR pulse_alarm_callback(
    gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
    void *user_context)
{
    (void)user_context;
    uint32_t delay_us;
    bool high;

    portENTER_CRITICAL_ISR(&state.lock);
    if (state.rpm == 0 || state.period_us == 0) {
        state.output_high = false;
        high = false;
        delay_us = 100000;
    } else if (!state.output_high) {
        state.output_high = true;
        high = true;
        delay_us = PULSE_HIGH_US;
    } else {
        state.output_high = false;
        high = false;
        delay_us = state.period_us > PULSE_HIGH_US
            ? state.period_us - PULSE_HIGH_US : 1;
    }
    portEXIT_CRITICAL_ISR(&state.lock);

    gpio_set_level(OUTPUT_GPIO, high);
    gptimer_alarm_config_t alarm = {
        .alarm_count = event->alarm_value + delay_us,
    };
    gptimer_set_alarm_action(timer, &alarm);
    return false;
}

static esp_err_t initialize_pulse_output(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << OUTPUT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "GPIO setup failed");
    gpio_set_level(OUTPUT_GPIO, 0);

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_config, &pulse_timer),
                        TAG, "timer creation failed");
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = pulse_alarm_callback,
    };
    ESP_RETURN_ON_ERROR(
        gptimer_register_event_callbacks(pulse_timer, &callbacks, NULL),
        TAG, "timer callback failed");
    ESP_RETURN_ON_ERROR(gptimer_enable(pulse_timer), TAG, "timer enable failed");
    gptimer_alarm_config_t alarm = {.alarm_count = 1};
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(pulse_timer, &alarm),
                        TAG, "timer alarm failed");
    return gptimer_start(pulse_timer);
}

static void print_help(void)
{
    puts("Commands:\n"
         "  rpm <0-4000>  Set a fixed engine speed\n"
         "  stop          Set engine speed to 0 RPM\n"
         "  sweep         Run 0 -> 4000 -> 0 RPM in 250 RPM steps\n"
         "  status        Show the current simulator state\n"
         "  help          Show this help");
}

static void print_status(void)
{
    portENTER_CRITICAL(&state.lock);
    uint32_t rpm = state.rpm;
    uint32_t period = state.period_us;
    bool high = state.output_high;
    bool sweeping = state.sweep_active;
    portEXIT_CRITICAL(&state.lock);
    printf("target=%lu RPM, period=%lu us, high=%u us, GPIO=%d, sweep=%s\n",
           (unsigned long)rpm, (unsigned long)period,
           high ? PULSE_HIGH_US : 0, OUTPUT_GPIO,
           sweeping ? "active" : "inactive");
}

static void sweep_task(void *argument)
{
    uint32_t generation = (uint32_t)(uintptr_t)argument;
    for (int rpm = 0; rpm <= (int)MAX_RPM; rpm += 250) {
        portENTER_CRITICAL(&state.lock);
        bool active = state.sweep_active &&
                      state.sweep_generation == generation;
        portEXIT_CRITICAL(&state.lock);
        if (!active) goto done;
        set_sweep_rpm((uint32_t)rpm);
        ESP_LOGI(TAG, "Sweep: %d RPM", rpm);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (int rpm = (int)MAX_RPM - 250; rpm >= 0; rpm -= 250) {
        portENTER_CRITICAL(&state.lock);
        bool active = state.sweep_active &&
                      state.sweep_generation == generation;
        portEXIT_CRITICAL(&state.lock);
        if (!active) goto done;
        set_sweep_rpm((uint32_t)rpm);
        ESP_LOGI(TAG, "Sweep: %d RPM", rpm);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    portENTER_CRITICAL(&state.lock);
    if (state.sweep_generation == generation) {
        state.sweep_active = false;
    }
    portEXIT_CRITICAL(&state.lock);
    set_sweep_rpm(0);
    ESP_LOGI(TAG, "Sweep complete; output stopped");
done:
    vTaskDelete(NULL);
}

static void start_sweep(void)
{
    portENTER_CRITICAL(&state.lock);
    state.sweep_active = true;
    uint32_t generation = ++state.sweep_generation;
    portEXIT_CRITICAL(&state.lock);
    if (xTaskCreate(sweep_task, "rpm_sweep", 4096,
                    (void *)(uintptr_t)generation, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&state.lock);
        state.sweep_active = false;
        portEXIT_CRITICAL(&state.lock);
        ESP_LOGE(TAG, "Unable to start sweep task");
        return;
    }
    ESP_LOGI(TAG, "Sweep started");
}

static void process_command(char *command)
{
    while (isspace((unsigned char)*command)) command++;
    char *end = command + strlen(command);
    while (end > command && isspace((unsigned char)end[-1])) *--end = '\0';

    if (strncmp(command, "rpm ", 4) == 0) {
        char *parse_end;
        unsigned long rpm = strtoul(command + 4, &parse_end, 10);
        while (isspace((unsigned char)*parse_end)) parse_end++;
        if (*parse_end || rpm > MAX_RPM) {
            ESP_LOGW(TAG, "Usage: rpm <0-4000>; output unchanged");
        } else {
            set_manual_rpm((uint32_t)rpm);
        }
    } else if (strcmp(command, "stop") == 0) {
        set_manual_rpm(0);
    } else if (strcmp(command, "sweep") == 0) {
        start_sweep();
    } else if (strcmp(command, "status") == 0) {
        print_status();
    } else if (strcmp(command, "help") == 0) {
        print_help();
    } else if (*command) {
        ESP_LOGW(TAG, "Unknown command '%s'; enter 'help'", command);
    }
}

static void console_task(void *argument)
{
    (void)argument;
    char line[128];
    while (true) {
        fputs("rpm-sim> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin)) {
            process_command(line);
        } else {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(initialize_pulse_output());
    set_manual_rpm(DEFAULT_RPM);
    ESP_LOGI(TAG, "Engine RPM simulator ready on GPIO%d; startup=%u RPM",
             OUTPUT_GPIO, DEFAULT_RPM);
    print_help();
    ESP_ERROR_CHECK(
        xTaskCreate(console_task, "rpm_console", 4096, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM);
}
