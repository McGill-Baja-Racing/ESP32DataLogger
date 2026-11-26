#include "app/modules_internal.h"

std::atomic<bool> s_sample_rpm_enabled{false};

void configure_rpm_sensor_adc_channels(void)
{
    ESP_ERROR_CHECK(adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(WHEEL_ADC_CH_USED, ADC_ATTEN_USED));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ENGINE_ADC_CH_USED, ADC_ATTEN_USED));
}

void sample_rpm_task(void *arg)
{
    /*
    This task samples the ADC pulse, detects each pulse crossing, and computes RPM
    from the time between the current pulse and the previous one.
    */

    const rpm_sensor_task_config_t *cfg = (const rpm_sensor_task_config_t *)arg;
    bool peak_detected = false;
    int64_t last_pulse_us = 0;
    int64_t last_delta_us = 0;
    int64_t last_log_us = 0;
    int64_t last_publish_us = 0;
    float last_published_rpm = -1.0f;
    int raw = 0;

    while (!s_sample_rpm_enabled) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    while (1)
    {
        if (!s_sample_rpm_enabled) {
            last_pulse_us = last_delta_us = last_publish_us = 0;
            peak_detected = false; last_published_rpm = -1;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        raw = adc1_get_raw(cfg->channel);
        const int64_t t_main_sample_us = esp_timer_get_time();

        // Detect one pulse when the signal crosses below the threshold.
        if (raw < RPM_PULSE_LOW_THRESHOLD && !peak_detected) {
            peak_detected = true;
            int64_t delta_us = 0;
            float rpm = 0.0f;

            if (last_pulse_us != 0) {
                delta_us = t_main_sample_us - last_pulse_us;
                if (delta_us > 0) {
                    last_delta_us = delta_us;
                    rpm = (float)(60000000.0 / ((double)delta_us * (double)cfg->pulses_per_rotation));
                }
            }

            last_pulse_us = t_main_sample_us;

            if (delta_us > 0 && (t_main_sample_us - last_log_us) >= 100000) {
                ESP_LOGI(TAG, "%s pulse detected | raw=%d dt=%lld us rpm=%.2f",
                         cfg->name, raw, delta_us, rpm);
                last_log_us = t_main_sample_us;
            }

            if (delta_us > 0 && (t_main_sample_us - last_publish_us) >= RPM_PUBLISH_INTERVAL_US) {
                const uint32_t timestamp_ms = (uint32_t)(t_main_sample_us / 1000);
                if (enqueue_replay_sensor_f32(cfg->sensor_id, timestamp_ms, rpm)) {
                    last_publish_us = t_main_sample_us;
                    last_published_rpm = rpm;
                }
            } else if (delta_us == 0) {
                ESP_LOGI(TAG, "%s first pulse detected | raw=%d waiting for next pulse",
                         cfg->name, raw);
            }
        }
        // Use a higher reset threshold to avoid retriggering on noisy edges.
        else if (raw > RPM_PULSE_RESET_THRESHOLD && peak_detected) {
            peak_detected = false;
        }

        int64_t zero_timeout_us = RPM_ZERO_TIMEOUT_MIN_US;
        if (cfg->hard_zero_timeout_us > 0) {
            zero_timeout_us = cfg->hard_zero_timeout_us;
        } else if (last_delta_us > 0) {
            const int64_t adaptive_timeout_us = last_delta_us * RPM_ZERO_TIMEOUT_MULTIPLIER;
            if (adaptive_timeout_us > zero_timeout_us) {
                zero_timeout_us = adaptive_timeout_us;
            }
        }

        if (last_pulse_us != 0 &&
            (t_main_sample_us - last_pulse_us) >= zero_timeout_us &&
            (t_main_sample_us - last_publish_us) >= RPM_PUBLISH_INTERVAL_US &&
            last_published_rpm != 0.0f) {
            const uint32_t timestamp_ms = (uint32_t)(t_main_sample_us / 1000);
            if (enqueue_replay_sensor_f32(cfg->sensor_id, timestamp_ms, 0.0f)) {
                last_publish_us = t_main_sample_us;
                last_published_rpm = 0.0f;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));
    }
}
