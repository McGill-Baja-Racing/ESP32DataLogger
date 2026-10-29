#include "sensor.h"

#include <math.h>

#include "adc_input.h"
#include "protocol/app_protocol.h"

#define THERMISTOR_GPIO         3
#define THERMOPILE_GPIO         2

#define V_IN_MV                 3300.0f
#define R_FIXED                 100000.0f
#define AMP_GAIN                247.0f   /* INA126 */
#define SENSITIVITY_MV_C        0.11f    /* ZTP-148SR */

#define R0                      100000.0f
#define T0                      298.15f
#define BETA                    3960.0f

static esp_err_t read_cvt_temperature(sensor_t *sensor, int32_t *value)
{
    (void)sensor;
    int thermistor_mv = 0;
    int thermopile_mv = 0;
    esp_err_t error = adc_input_read_mv(THERMISTOR_GPIO, &thermistor_mv);
    if (error != ESP_OK) {
        return error;
    }
    error = adc_input_read_mv(THERMOPILE_GPIO, &thermopile_mv);
    if (error != ESP_OK) {
        return error;
    }
    if (thermistor_mv <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    float r_therm = R_FIXED * ((V_IN_MV / (float)thermistor_mv) - 1.0f);
    if (r_therm <= 0.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    float temp_kelvin = 1.0f / ((1.0f / T0) + (1.0f / BETA) * logf(r_therm / R0));
    float ambient_temp_c = temp_kelvin - 273.15f;

    float true_thermopile_mv = (float)thermopile_mv / AMP_GAIN;
    float delta_t = true_thermopile_mv / SENSITIVITY_MV_C;
    /* The thermopile measures the target relative to its own (ambient) temperature. */
    float target_temp_c = ambient_temp_c + delta_t;
    *value = target_temp_c > 0.0f ? (int32_t)target_temp_c : 0;
    return ESP_OK;
}

sensor_t cvt_belt_sensor = {
    .name = "cvt_belt_temperature",
    .can_id = CAN_ID_CVT_TEMP,
    .period_us = 100000, /* 10 Hz */
    .read = read_cvt_temperature,
};
