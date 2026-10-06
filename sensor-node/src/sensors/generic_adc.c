#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

#if NODE_FIXED_ADC_CONFIG

/* First-pass general analog channel: ESP32-C3 ADC1 GPIO1, reported in mV. */
#define GENERIC_ADC_GPIO 1

static esp_err_t read_voltage_mv(sensor_t *sensor, int32_t *value)
{
    (void)sensor;
    int voltage_mv = 0;
    esp_err_t error = adc_input_read_mv(GENERIC_ADC_GPIO, &voltage_mv);
    if (error == ESP_OK) {
        *value = voltage_mv;
    }
    return error;
}

sensor_t generic_adc_sensor = {
    .name = "generic_adc_voltage",
    .can_id = CAN_ID_GENERIC_ADC,
    .period_us = 10000, /* 100 Hz */
    .read = read_voltage_mv,
};

#endif /* NODE_FIXED_ADC_CONFIG */
