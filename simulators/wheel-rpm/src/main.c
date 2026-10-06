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
#include "freertos/semphr.h"
#include "freertos/task.h"

#define OUTPUT_GPIO_A          GPIO_NUM_6
#define OUTPUT_GPIO_B          GPIO_NUM_7
#define PULSES_PER_REVOLUTION  32U
#define COUNTS_PER_REVOLUTION  (PULSES_PER_REVOLUTION * 4U)
#define DEFAULT_RPM            500
#define MAX_ABSOLUTE_RPM       3000
#define TIMER_RESOLUTION_HZ    1000000U

typedef struct {
    int32_t rpm;
    uint32_t absolute_rpm;
    int8_t direction;
    uint8_t state_index;
    uint64_t interval_denominator;
    uint64_t fractional_accumulator;
    portMUX_TYPE lock;
} encoder_state_t;

typedef struct {
    bool active;
    uint32_t generation;
    int32_t start;
    int32_t end;
    int32_t step;
    uint32_t dwell_ms;
} sweep_state_t;

static const char *TAG = "WheelRPM";
static const uint8_t quadrature_states[4] = {0, 1, 3, 2};
static gptimer_handle_t edge_timer;
static SemaphoreHandle_t control_mutex;
static encoder_state_t encoder = {
    .rpm = DEFAULT_RPM,
    .absolute_rpm = DEFAULT_RPM,
    .direction = 1,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static sweep_state_t sweep;

static void IRAM_ATTR write_quadrature_state(uint8_t index)
{
    uint8_t state = quadrature_states[index & 3U];
    gpio_set_level(OUTPUT_GPIO_A, (state >> 1) & 1U);
    gpio_set_level(OUTPUT_GPIO_B, state & 1U);
}

static uint32_t IRAM_ATTR next_interval_locked(void)
{
    if (encoder.interval_denominator == 0) return 100000;
    uint64_t base = 60000000ULL / encoder.interval_denominator;
    uint64_t remainder = 60000000ULL % encoder.interval_denominator;
    encoder.fractional_accumulator += remainder;
    if (encoder.fractional_accumulator >= encoder.interval_denominator) {
        encoder.fractional_accumulator -= encoder.interval_denominator;
        base++;
    }
    return base ? (uint32_t)base : 1;
}

static bool IRAM_ATTR edge_alarm_callback(
    gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
    void *user_context)
{
    (void)user_context;
    uint32_t interval;
    uint8_t state_index;
    portENTER_CRITICAL_ISR(&encoder.lock);
    if (encoder.absolute_rpm) {
        encoder.state_index =
            (uint8_t)((encoder.state_index + encoder.direction + 4) & 3);
    }
    state_index = encoder.state_index;
    interval = next_interval_locked();
    portEXIT_CRITICAL_ISR(&encoder.lock);
    write_quadrature_state(state_index);
    gptimer_alarm_config_t alarm = {
        .alarm_count = event->alarm_value + interval,
    };
    gptimer_set_alarm_action(timer, &alarm);
    return false;
}

static esp_err_t init_outputs(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << OUTPUT_GPIO_A) | (1ULL << OUTPUT_GPIO_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "output setup failed");
    write_quadrature_state(0);
    return ESP_OK;
}

static esp_err_t init_edge_timer(void)
{
    gptimer_config_t config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&config, &edge_timer),
                        TAG, "timer creation failed");
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = edge_alarm_callback,
    };
    ESP_RETURN_ON_ERROR(
        gptimer_register_event_callbacks(edge_timer, &callbacks, NULL),
        TAG, "timer callback failed");
    ESP_RETURN_ON_ERROR(gptimer_enable(edge_timer), TAG, "timer enable failed");
    gptimer_alarm_config_t alarm = {.alarm_count = 1};
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(edge_timer, &alarm),
                        TAG, "timer alarm failed");
    return gptimer_start(edge_timer);
}

static void set_encoder_rpm(int32_t rpm)
{
    if (rpm > MAX_ABSOLUTE_RPM) rpm = MAX_ABSOLUTE_RPM;
    if (rpm < -MAX_ABSOLUTE_RPM) rpm = -MAX_ABSOLUTE_RPM;
    uint32_t absolute = rpm < 0 ? (uint32_t)(-rpm) : (uint32_t)rpm;

    portENTER_CRITICAL(&encoder.lock);
    encoder.rpm = rpm;
    encoder.absolute_rpm = absolute;
    encoder.direction = rpm < 0 ? -1 : 1;
    encoder.interval_denominator =
        (uint64_t)absolute * COUNTS_PER_REVOLUTION;
    encoder.fractional_accumulator = 0;
    portEXIT_CRITICAL(&encoder.lock);
}

static void cancel_sweep(void)
{
    xSemaphoreTake(control_mutex, portMAX_DELAY);
    sweep.active = false;
    sweep.generation++;
    xSemaphoreGive(control_mutex);
}

static void print_help(void)
{
    puts("Commands:\n"
         "  rpm <value>                         signed RPM (-3000..3000)\n"
         "  stop                                stop and cancel sweep\n"
         "  sweep <start> <end> <step> <dwell> run one sweep; dwell is ms\n"
         "  cancel                              cancel sweep, retain RPM\n"
         "  status                              show current state\n"
         "  help                                show this help");
}

static void print_status(void)
{
    portENTER_CRITICAL(&encoder.lock);
    int32_t rpm = encoder.rpm;
    uint8_t index = encoder.state_index;
    portEXIT_CRITICAL(&encoder.lock);
    xSemaphoreTake(control_mutex, portMAX_DELAY);
    sweep_state_t current = sweep;
    xSemaphoreGive(control_mutex);
    const char *direction = rpm > 0 ? "forward" : rpm < 0 ? "reverse" : "stopped";
    uint8_t state = quadrature_states[index & 3U];
    printf("RPM=%ld direction=%s A=%u B=%u pins=A:GPIO%d/B:GPIO%d sweep=%s",
           (long)rpm, direction, (state >> 1) & 1U, state & 1U,
           OUTPUT_GPIO_A, OUTPUT_GPIO_B,
           current.active ? "active" : "inactive");
    if (current.active) {
        printf(" end=%ld step=%ld dwell=%lums", (long)current.end,
               (long)current.step, (unsigned long)current.dwell_ms);
    }
    putchar('\n');
}

static bool parse_int32(const char *text, int32_t *value)
{
    if (!text || !*text) return false;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (*end || parsed < INT32_MIN || parsed > INT32_MAX) return false;
    *value = (int32_t)parsed;
    return true;
}

static void sweep_task(void *argument)
{
    (void)argument;
    while (true) {
        xSemaphoreTake(control_mutex, portMAX_DELAY);
        sweep_state_t current = sweep;
        xSemaphoreGive(control_mutex);
        if (!current.active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int32_t rpm = current.start;
        while (true) {
            xSemaphoreTake(control_mutex, portMAX_DELAY);
            bool active = sweep.active &&
                          sweep.generation == current.generation;
            xSemaphoreGive(control_mutex);
            if (!active) break;
            set_encoder_rpm(rpm);
            ESP_LOGI(TAG, "Sweep: RPM=%ld", (long)rpm);
            vTaskDelay(pdMS_TO_TICKS(current.dwell_ms));
            if (rpm == current.end) {
                xSemaphoreTake(control_mutex, portMAX_DELAY);
                if (sweep.generation == current.generation) sweep.active = false;
                xSemaphoreGive(control_mutex);
                set_encoder_rpm(0);
                ESP_LOGI(TAG, "Sweep complete; stopped");
                break;
            }
            int64_t next = (int64_t)rpm + current.step;
            rpm = current.step > 0 && next > current.end ? current.end :
                  current.step < 0 && next < current.end ? current.end :
                  (int32_t)next;
        }
    }
}

static void handle_command(char *line)
{
    char *save;
    char *command = strtok_r(line, " \t\r\n", &save);
    if (!command) return;
    if (strcmp(command, "rpm") == 0) {
        char *value_text = strtok_r(NULL, " \t\r\n", &save);
        int32_t rpm;
        if (!parse_int32(value_text, &rpm) ||
            strtok_r(NULL, " \t\r\n", &save) ||
            rpm < -MAX_ABSOLUTE_RPM || rpm > MAX_ABSOLUTE_RPM) {
            puts("error: usage: rpm <-3000..3000>");
            return;
        }
        cancel_sweep();
        set_encoder_rpm(rpm);
        printf("RPM set to %ld\n", (long)rpm);
    } else if (strcmp(command, "sweep") == 0) {
        int32_t start, end, step, dwell;
        char *a = strtok_r(NULL, " \t\r\n", &save);
        char *b = strtok_r(NULL, " \t\r\n", &save);
        char *c = strtok_r(NULL, " \t\r\n", &save);
        char *d = strtok_r(NULL, " \t\r\n", &save);
        if (!parse_int32(a, &start) || !parse_int32(b, &end) ||
            !parse_int32(c, &step) || !parse_int32(d, &dwell) ||
            strtok_r(NULL, " \t\r\n", &save) ||
            start < -MAX_ABSOLUTE_RPM || start > MAX_ABSOLUTE_RPM ||
            end < -MAX_ABSOLUTE_RPM || end > MAX_ABSOLUTE_RPM ||
            step == 0 || dwell <= 0 ||
            (start < end && step < 0) || (start > end && step > 0)) {
            puts("error: usage: sweep <start> <end> <step> <dwell_ms>");
            return;
        }
        xSemaphoreTake(control_mutex, portMAX_DELAY);
        sweep.active = true;
        sweep.generation++;
        sweep.start = start;
        sweep.end = end;
        sweep.step = step;
        sweep.dwell_ms = (uint32_t)dwell;
        xSemaphoreGive(control_mutex);
        set_encoder_rpm(start);
        printf("Sweep started at %ld RPM\n", (long)start);
    } else if (strcmp(command, "stop") == 0) {
        cancel_sweep();
        set_encoder_rpm(0);
        puts("Stopped");
    } else if (strcmp(command, "cancel") == 0) {
        cancel_sweep();
        puts("Sweep cancelled; current RPM retained");
    } else if (strcmp(command, "status") == 0) {
        print_status();
    } else if (strcmp(command, "help") == 0) {
        print_help();
    } else {
        puts("error: unknown command or unexpected arguments; type 'help'");
    }
}

static void console_task(void *argument)
{
    (void)argument;
    char line[128];
    while (true) {
        if (fgets(line, sizeof(line), stdin)) {
            handle_command(line);
        } else {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void app_main(void)
{
    control_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(control_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(init_outputs());
    ESP_ERROR_CHECK(init_edge_timer());
    set_encoder_rpm(DEFAULT_RPM);
    ESP_LOGI(TAG, "SKF BMB-6202/032S2/UB108A simulator ready");
    ESP_LOGI(TAG, "%u PPR, %u quadrature counts/revolution",
             PULSES_PER_REVOLUTION, COUNTS_PER_REVOLUTION);
    ESP_LOGI(TAG, "A=GPIO%d, B=GPIO%d, startup RPM=%d",
             OUTPUT_GPIO_A, OUTPUT_GPIO_B, DEFAULT_RPM);
    print_help();
    ESP_ERROR_CHECK(
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(
        xTaskCreate(sweep_task, "sweep", 3072, NULL, 4, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM);
}
