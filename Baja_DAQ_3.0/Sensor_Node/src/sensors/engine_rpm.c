#include "sensor.h"

#include "esp_log.h"
#include "protocol/app_protocol.h"

/*
 * Engine RPM implementation placeholder
 * -------------------------------------
 * Planned input: raw analog tach signal on ESP32-C3 ADC1 GPIO3.
 * Planned algorithm: detect voltage peaks with hysteresis, reject pulses that
 * exceed the physical RPM limit, calculate RPM from peak spacing and pulses
 * per revolution, and return zero after a configurable no-pulse timeout.
 *
 * Hardware testing must determine the detection/reset thresholds, polarity,
 * pulses per revolution, valid RPM range, filtering, and timeout. Until those
 * values are validated this driver intentionally reports zero; it must not be
 * interpreted as a functioning RPM measurement.
 */
#define ENGINE_RPM_GPIO 3

static esp_err_t init_engine_rpm(sensor_t *sensor)
{
    (void)sensor;
    ESP_LOGW("EngineRPM",
             "Placeholder enabled on GPIO%d; RPM output is not implemented",
             ENGINE_RPM_GPIO);
    return ESP_OK;
}

static int32_t read_engine_rpm(sensor_t *sensor)
{
    (void)sensor;
    /* TODO: implement and validate analog voltage-peak detection. */
    return 0;
}

sensor_t engine_rpm_sensor = {
    .name = "engine_rpm",
    .can_id = CAN_ID_ENGINE_RPM,
    .period_us = 20000, /* 50 Hz reporting rate */
    .init = init_engine_rpm,
    .read = read_engine_rpm,
};
