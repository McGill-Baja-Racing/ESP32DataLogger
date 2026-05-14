#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define LED_STRIP_GPIO      6
#define LED_STRIP_LED_COUNT 9   // Start with 9 for one fan. Try 45 if 5 fans are chained.
#define LED_STRIP_RES_HZ    10000000

#define FAN_PWM_GPIO        7
#define FAN_PWM_FREQ_HZ     25000
#define FAN_PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define FAN_PWM_TIMER       LEDC_TIMER_0
#define FAN_PWM_CHANNEL     LEDC_CHANNEL_0
#define FAN_PWM_MODE        LEDC_LOW_SPEED_MODE
#define FAN_PWM_MAX_DUTY    255

static const char *TAG = "ARGB_TEST";

static void fan_pwm_init(void)
{
    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = FAN_PWM_MODE;
    timer_config.duty_resolution = FAN_PWM_RESOLUTION;
    timer_config.timer_num = FAN_PWM_TIMER;
    timer_config.freq_hz = FAN_PWM_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {};
    channel_config.gpio_num = FAN_PWM_GPIO;
    channel_config.speed_mode = FAN_PWM_MODE;
    channel_config.channel = FAN_PWM_CHANNEL;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = FAN_PWM_TIMER;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
}

static void fan_pwm_set_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (FAN_PWM_MAX_DUTY * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL));
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing A-RGB strip");
    fan_pwm_init();
    fan_pwm_set_percent(50);
    ESP_LOGI(TAG, "Fan PWM initialized on GPIO%d at %d Hz", FAN_PWM_GPIO, FAN_PWM_FREQ_HZ);

    led_strip_handle_t led_strip;

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RES_HZ,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        }
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    while (true) {
        /*
        ESP_LOGI(TAG, "Red, fan 25%%");
        fan_pwm_set_percent(25);
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            led_strip_set_pixel(led_strip, i, 255, 0, 0);
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(10000));

        ESP_LOGI(TAG, "Green, fan 50%%");
        fan_pwm_set_percent(50);
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            led_strip_set_pixel(led_strip, i, 0, 255, 0);
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(10000));

        ESP_LOGI(TAG, "Blue, fan 75%%");
        fan_pwm_set_percent(75);
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            led_strip_set_pixel(led_strip, i, 0, 0, 255);
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(10000));
        */
        ESP_LOGI(TAG, "Rainbow, fan 100%%");
        fan_pwm_set_percent(100);
        for (int j = 0; j < 255; j += 5) {
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                uint8_t hue = j + i * 20;

                uint8_t r = (hue < 85) ? 255 - hue * 3 : (hue < 170) ? 0 : (hue - 170) * 3;
                uint8_t g = (hue < 85) ? hue * 3 : (hue < 170) ? 255 - (hue - 85) * 3 : 0;
                uint8_t b = (hue < 85) ? 0 : (hue < 170) ? (hue - 85) * 3 : 255 - (hue - 170) * 3;

                led_strip_set_pixel(led_strip, i, r, g, b);
            }

            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
