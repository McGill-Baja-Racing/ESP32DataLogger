#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

#define GPIO                    1
#define DIVIDER_SCALE           (2.33f / 4.33f)
#define SENSOR_MIN_MV           500.0f
#define SENSOR_MAX_MV           4500.0f
#define PRESSURE_SPAN_PSI       3000.0f

static esp_err_t read_pressure(sensor_t *sensor, int32_t *value)
{
    (void)sensor;
    int voltage_mv = 0;
    esp_err_t error = adc_input_read_mv(GPIO, &voltage_mv);
    if (error != ESP_OK) {
        return error;
    }
    const float minimum_mv = SENSOR_MIN_MV * DIVIDER_SCALE;
    const float span_mv = (SENSOR_MAX_MV - SENSOR_MIN_MV) * DIVIDER_SCALE;
    float pressure = ((voltage_mv - minimum_mv) / span_mv) * PRESSURE_SPAN_PSI;
    *value = pressure > 0.0f ? (int32_t)pressure : 0;
    return ESP_OK;
}

sensor_t front_brake_sensor = {
    .name = "front_brake_pressure",
    .can_id = CAN_ID_FRONT_BRAKE,
    .period_us = 10000,
    .read = read_pressure,
};
