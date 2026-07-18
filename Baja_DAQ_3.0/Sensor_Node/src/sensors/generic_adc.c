#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

/* First-pass general analog channel: ESP32-C3 ADC1 GPIO1, reported in mV. */
#define GENERIC_ADC_GPIO 1

static int32_t read_voltage_mv(sensor_t *sensor)
{
    (void)sensor;
    int voltage_mv = 0;
    return adc_input_read_mv(GENERIC_ADC_GPIO, &voltage_mv) == ESP_OK
         ? voltage_mv : 0;
}

sensor_t generic_adc_sensor = {
    .name = "generic_adc_voltage",
    .can_id = CAN_ID_GENERIC_ADC,
    .period_us = 10000, /* 100 Hz */
    .read = read_voltage_mv,
};
