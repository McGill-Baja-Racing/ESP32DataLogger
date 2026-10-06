#include "adc_input.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "ADC";
static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t calibration[5];
static bool configured[5];

esp_err_t adc_input_init(void)
{
    if (adc_handle) {
        return ESP_OK;
    }
    adc_oneshot_unit_init_cfg_t config = {.unit_id = ADC_UNIT_1};
    return adc_oneshot_new_unit(&config, &adc_handle);
}

static esp_err_t configure_channel(adc_channel_t channel)
{
    if (configured[channel]) {
        return ESP_OK;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t error = adc_oneshot_config_channel(adc_handle, channel, &channel_config);
    if (error != ESP_OK) {
        return error;
    }
    configured[channel] = true;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                  &calibration[channel]);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_cali_create_scheme_line_fitting(&calibration_config,
                                                 &calibration[channel]);
#else
    error = ESP_ERR_NOT_SUPPORTED;
#endif
    if (error != ESP_OK) {
        calibration[channel] = NULL;
        ESP_LOGW(TAG, "Calibration unavailable on GPIO%d", channel);
    }
    return ESP_OK;
}

esp_err_t adc_input_read_mv(uint8_t gpio, int *voltage_mv)
{
    if (!adc_handle || !voltage_mv || gpio > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_channel_t channel = (adc_channel_t)gpio;
    esp_err_t error = configure_channel(channel);
    if (error != ESP_OK) {
        return error;
    }

    int raw;
    error = adc_oneshot_read(adc_handle, channel, &raw);
    if (error != ESP_OK) {
        return error;
    }
    if (calibration[channel]) {
        return adc_cali_raw_to_voltage(calibration[channel], raw, voltage_mv);
    }
    *voltage_mv = (raw * 2450) / 4095;
    return ESP_OK;
}
