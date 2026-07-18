#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

#define GPIO                    2
#define DIVIDER_SCALE           (2.33f / 4.33f)
#define SENSOR_MIN_MV           500.0f
#define SENSOR_MAX_MV           4500.0f
#define PRESSURE_SPAN_PSI       1600.0f

static int32_t read_pressure(sensor_t *sensor)
{
    (void)sensor;
    int voltage_mv = 0;
    if (adc_input_read_mv(GPIO, &voltage_mv) != ESP_OK) {
        return 0;
    }
    const float minimum_mv = SENSOR_MIN_MV * DIVIDER_SCALE;
    const float span_mv = (SENSOR_MAX_MV - SENSOR_MIN_MV) * DIVIDER_SCALE;
    float pressure = ((voltage_mv - minimum_mv) / span_mv) * PRESSURE_SPAN_PSI;
    return pressure > 0.0f ? (int32_t)pressure : 0;
}

sensor_t rear_brake_sensor = {
    .name = "rear_brake_pressure",
    .can_id = CAN_ID_REAR_BRAKE,
    .period_us = 10000,
    .read = read_pressure,
};
