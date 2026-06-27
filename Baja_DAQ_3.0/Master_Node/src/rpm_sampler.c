#include "rpm_sampler.h"

#include <inttypes.h>
#include <string.h>
#include <strings.h>

#include "driver/adc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "master_control.h"

static const char *TAG = "rpm_sampler";

#define RPM_MAX_SIGNALS 4
#define RPM_PULSES_PER_ROTATION 1
#define RPM_MAX_VALID 8000
#define RPM_ZERO_TIMEOUT_MIN_US 250000LL
#define RPM_ZERO_TIMEOUT_MULTIPLIER 3
#define RPM_TASK_POLL_MS 2
#define RPM_PULSE_LOW_THRESHOLD 1000
#define RPM_PULSE_RESET_THRESHOLD 1200

typedef struct {
    rpm_signal_config_t config;
    int64_t last_pulse_us;
    int64_t last_delta_us;
    int64_t last_publish_us;
    int32_t last_value;
    int32_t last_published_value;
    adc1_channel_t adc_channel;
    bool peak_detected;
    bool configured;
} rpm_runtime_signal_t;

static TaskHandle_t s_rpm_task_handle;
static rpm_runtime_signal_t s_rpm_signals[RPM_MAX_SIGNALS];
static size_t s_rpm_signal_count;
static portMUX_TYPE s_rpm_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_adc_width_configured;

static bool rpm_function_is_supported(const char *function)
{
    return function &&
           (strcasecmp(function, "engine_rpm") == 0 ||
            strcasecmp(function, "rpm") == 0);
}

static bool gpio_to_adc1_channel(uint8_t gpio, adc1_channel_t *out_channel)
{
    if (!out_channel) {
        return false;
    }

    switch (gpio) {
    case 16:
        *out_channel = ADC1_CHANNEL_0;
        return true;
    case 17:
        *out_channel = ADC1_CHANNEL_1;
        return true;
    case 18:
        *out_channel = ADC1_CHANNEL_2;
        return true;
    case 19:
        *out_channel = ADC1_CHANNEL_3;
        return true;
    case 20:
        *out_channel = ADC1_CHANNEL_4;
        return true;
    case 21:
        *out_channel = ADC1_CHANNEL_5;
        return true;
    case 22:
        *out_channel = ADC1_CHANNEL_6;
        return true;
    case 23:
        *out_channel = ADC1_CHANNEL_7;
        return true;
    default:
        return false;
    }
}

static bool configure_rpm_adc(rpm_runtime_signal_t *signal)
{
    if (!signal || !gpio_to_adc1_channel(signal->config.gpio, &signal->adc_channel)) {
        ESP_LOGW(TAG,
                 "RPM GPIO%u is not an ADC1 input on ESP32-P4. Use GPIO16-23.",
                 signal ? signal->config.gpio : 0);
        return false;
    }

    if (!s_adc_width_configured) {
        esp_err_t width_err = adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_DEFAULT);
        if (width_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure RPM ADC width: %s", esp_err_to_name(width_err));
            return false;
        }
        s_adc_width_configured = true;
    }

    esp_err_t atten_err = adc1_config_channel_atten(signal->adc_channel, ADC_ATTEN_DB_11);
    if (atten_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to configure RPM ADC GPIO%u channel=%d: %s",
                 signal->config.gpio,
                 signal->adc_channel,
                 esp_err_to_name(atten_err));
        return false;
    }

    ESP_LOGI(TAG,
             "Configured local RPM ADC can_id=0x%03" PRIX32
             " GPIO%u ADC1_CHANNEL_%d low=%d reset=%d",
             signal->config.can_id,
             signal->config.gpio,
             signal->adc_channel,
             RPM_PULSE_LOW_THRESHOLD,
             RPM_PULSE_RESET_THRESHOLD);
    return true;
}

void rpm_sampler_configure_signals(const rpm_signal_config_t *signals, size_t signal_count)
{
    if (signal_count > RPM_MAX_SIGNALS) {
        signal_count = RPM_MAX_SIGNALS;
    }

    portENTER_CRITICAL(&s_rpm_lock);
    memset(s_rpm_signals, 0, sizeof(s_rpm_signals));

    size_t out = 0;
    for (size_t i = 0; i < signal_count && out < RPM_MAX_SIGNALS; i++) {
        if (!signals[i].enabled ||
            signals[i].can_id == 0 ||
            !rpm_function_is_supported(signals[i].function)) {
            continue;
        }
        s_rpm_signals[out].config = signals[i];
        if (s_rpm_signals[out].config.sample_rate_hz == 0) {
            s_rpm_signals[out].config.sample_rate_hz = 10;
        }
        s_rpm_signals[out].last_published_value = -1;
        out++;
    }
    s_rpm_signal_count = out;
    portEXIT_CRITICAL(&s_rpm_lock);

    for (size_t i = 0; i < s_rpm_signal_count; i++) {
        bool configured = configure_rpm_adc(&s_rpm_signals[i]);
        portENTER_CRITICAL(&s_rpm_lock);
        if (i < s_rpm_signal_count) {
            s_rpm_signals[i].configured = configured;
        }
        portEXIT_CRITICAL(&s_rpm_lock);
    }
}

static void rpm_sampler_task(void *arg)
{
    (void)arg;

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        rpm_runtime_signal_t snapshot[RPM_MAX_SIGNALS] = {};
        size_t signal_count = 0;

        portENTER_CRITICAL(&s_rpm_lock);
        signal_count = s_rpm_signal_count;
        memcpy(snapshot, s_rpm_signals, sizeof(snapshot));
        portEXIT_CRITICAL(&s_rpm_lock);

        for (size_t i = 0; i < signal_count; i++) {
            rpm_runtime_signal_t *signal = &snapshot[i];
            if (!signal->configured || !signal->config.enabled) {
                continue;
            }

            int raw = adc1_get_raw(signal->adc_channel);
            if (raw < 0) {
                ESP_LOGW(TAG,
                         "RPM ADC read failed GPIO%u channel=%d raw=%d",
                         signal->config.gpio,
                         signal->adc_channel,
                         raw);
                continue;
            }

            int32_t value = signal->last_value;
            bool peak_detected = signal->peak_detected;
            int64_t last_pulse_us = signal->last_pulse_us;
            int64_t last_delta_us = signal->last_delta_us;
            const int64_t min_interval_us =
                60000000LL / (RPM_MAX_VALID * RPM_PULSES_PER_ROTATION);

            if (raw < RPM_PULSE_LOW_THRESHOLD && !peak_detected) {
                peak_detected = true;
                if (last_pulse_us != 0) {
                    int64_t delta_us = now_us - last_pulse_us;
                    if (delta_us >= min_interval_us) {
                        last_delta_us = delta_us;
                        value = (int32_t)(60000000LL /
                                          (delta_us * RPM_PULSES_PER_ROTATION));
                    }
                }
                last_pulse_us = now_us;
            } else if (raw > RPM_PULSE_RESET_THRESHOLD && peak_detected) {
                peak_detected = false;
            }

            int64_t zero_timeout_us = RPM_ZERO_TIMEOUT_MIN_US;
            if (last_delta_us > 0) {
                int64_t adaptive_timeout_us =
                    last_delta_us * RPM_ZERO_TIMEOUT_MULTIPLIER;
                if (adaptive_timeout_us > zero_timeout_us) {
                    zero_timeout_us = adaptive_timeout_us;
                }
            }

            if (last_pulse_us == 0 ||
                (now_us - last_pulse_us) >= zero_timeout_us) {
                value = 0;
            }

            uint16_t rate_hz = signal->config.sample_rate_hz ?
                               signal->config.sample_rate_hz :
                               10;
            int64_t publish_period_us = 1000000LL / rate_hz;
            if (publish_period_us <= 0) {
                publish_period_us = 1000;
            }

            portENTER_CRITICAL(&s_rpm_lock);
            if (i < s_rpm_signal_count &&
                s_rpm_signals[i].config.can_id == signal->config.can_id) {
                s_rpm_signals[i].peak_detected = peak_detected;
                s_rpm_signals[i].last_pulse_us = last_pulse_us;
                s_rpm_signals[i].last_delta_us = last_delta_us;
                s_rpm_signals[i].last_value = value;
            }
            portEXIT_CRITICAL(&s_rpm_lock);

            if ((now_us - signal->last_publish_us) < publish_period_us &&
                value == signal->last_published_value) {
                continue;
            }

            uint32_t timestamp_ms = (uint32_t)(now_us / 1000);
            (void)master_submit_local_sample(signal->config.can_id,
                                             (uint32_t)value,
                                             timestamp_ms,
                                             signal->config.preview_enabled);

            portENTER_CRITICAL(&s_rpm_lock);
            if (i < s_rpm_signal_count &&
                s_rpm_signals[i].config.can_id == signal->config.can_id) {
                s_rpm_signals[i].last_publish_us = now_us;
                s_rpm_signals[i].last_published_value = value;
            }
            portEXIT_CRITICAL(&s_rpm_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(RPM_TASK_POLL_MS));
    }
}

esp_err_t rpm_sampler_start(void)
{
    if (s_rpm_task_handle) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(rpm_sampler_task,
                                "rpm_sampler",
                                4096,
                                NULL,
                                4,
                                &s_rpm_task_handle);
    if (ok != pdPASS) {
        s_rpm_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
