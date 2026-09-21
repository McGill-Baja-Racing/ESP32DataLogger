#include "sensor.h"

/*
 * This is the only file that decides which sensors belong to a node build.
 * Drivers remain independent and export descriptors consumed here.
 */

#if NODE_FIXED_BRAKE_CONFIG
extern sensor_t front_brake_sensor;
extern sensor_t rear_brake_sensor;

static sensor_t sensors[2];
#elif NODE_FIXED_ENCODER_CONFIG
extern sensor_t bearing_encoder_sensor;

static sensor_t sensors[1];
#elif NODE_FIXED_ENGINE_CONFIG
extern sensor_t engine_rpm_sensor;

static sensor_t sensors[1];
#elif NODE_FIXED_ADC_CONFIG
extern sensor_t generic_adc_sensor;

static sensor_t sensors[1];
#elif NODE_FIXED_MPU_CONFIG
extern sensor_t mpu_accel_x_sensor;
extern sensor_t mpu_accel_y_sensor;
extern sensor_t mpu_accel_z_sensor;
extern sensor_t mpu_gyro_x_sensor;
extern sensor_t mpu_gyro_y_sensor;
extern sensor_t mpu_gyro_z_sensor;

static sensor_t sensors[6];
#else
#error "Select a fixed sensor-node configuration"
#endif

sensor_t *sensor_registry(size_t *count)
{
#if NODE_FIXED_BRAKE_CONFIG
    sensors[0] = front_brake_sensor;
    sensors[1] = rear_brake_sensor;
#elif NODE_FIXED_ENCODER_CONFIG
    sensors[0] = bearing_encoder_sensor;
#elif NODE_FIXED_ENGINE_CONFIG
    sensors[0] = engine_rpm_sensor;
#elif NODE_FIXED_MPU_CONFIG
    sensors[0] = mpu_accel_x_sensor;
    sensors[1] = mpu_accel_y_sensor;
    sensors[2] = mpu_accel_z_sensor;
    sensors[3] = mpu_gyro_x_sensor;
    sensors[4] = mpu_gyro_y_sensor;
    sensors[5] = mpu_gyro_z_sensor;
#else
    sensors[0] = generic_adc_sensor;
#endif
    *count = sizeof(sensors) / sizeof(sensors[0]);
    return sensors;
}
