#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

#if NODE_FIXED_ADC_CONFIG

/* Two linear pots in series on one ADC1 channel, reported in micrometres. */
#define GPIO               0          /* free on the ADC node; GPIO1 is generic_adc */
#define SHOCK_MIN_MV       100.0f     /* TODO: pin voltage at full compression */
#define SHOCK_MAX_MV       2300.0f    /* TODO: pin voltage at full extension, keep < 2450 */
#define STROKE_UM          100000.0f  /* TODO: total stroke from datasheet */

static esp_err_t read_displacement(sensor_t *sensor, int32_t *value)
{
    (void)sensor;
    int voltage_mv = 0;
    esp_err_t error = adc_input_read_mv(GPIO, &voltage_mv);
    if (error != ESP_OK) {
        return error;
    }
    float displacement = ((voltage_mv - SHOCK_MIN_MV) / (SHOCK_MAX_MV - SHOCK_MIN_MV)) * STROKE_UM;
    *value = displacement > 0.0f ? (int32_t)displacement : 0;
    return ESP_OK;
}

sensor_t shock_pot_sensor = {
    .name = "shock_displacement",
    .can_id = CAN_ID_SHOCK_DISPLACEMENT,
    .period_us = 10000, /* 100 Hz */
    .read = read_displacement,
};

#endif /* NODE_FIXED_ADC_CONFIG */
