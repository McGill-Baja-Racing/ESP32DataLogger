#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "sensor.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"

/* MPU-6500 and MPU-9250 share this accelerometer/gyroscope register map. */
#define MPU_I2C_PORT               0
#define MPU_I2C_SDA_GPIO           4
#define MPU_I2C_SCL_GPIO           5
#define MPU_I2C_ADDRESS_AD0_LOW    0x68
#define MPU_I2C_ADDRESS_AD0_HIGH   0x69
#define MPU_I2C_CLOCK_HZ           400000
#define MPU_SAMPLE_PERIOD_US       10000
#define MPU_TRANSFER_TIMEOUT_MS    20

#define MPU_REG_SMPLRT_DIV         0x19
#define MPU_REG_CONFIG             0x1A
#define MPU_REG_GYRO_CONFIG        0x1B
#define MPU_REG_ACCEL_CONFIG       0x1C
#define MPU_REG_ACCEL_CONFIG_2     0x1D
#define MPU_REG_ACCEL_XOUT_H       0x3B
#define MPU_REG_PWR_MGMT_1         0x6B
#define MPU_REG_WHO_AM_I           0x75

#define MPU_WHO_AM_I_6500          0x70
#define MPU_WHO_AM_I_9250          0x71

typedef enum {
    MPU_ACCEL_X,
    MPU_ACCEL_Y,
    MPU_ACCEL_Z,
    MPU_GYRO_X,
    MPU_GYRO_Y,
    MPU_GYRO_Z,
    MPU_CHANNEL_COUNT,
} mpu_channel_t;

typedef struct {
    mpu_channel_t channel;
} mpu_sensor_context_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    int32_t values[MPU_CHANNEL_COUNT];
    uint8_t address;
    bool initialized;
    esp_err_t sample_status;
} mpu_device_context_t;

static const char *TAG = "MPU6500";
static mpu_device_context_t mpu;

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[2] = {reg, value};
    return i2c_master_transmit(mpu.device, command, sizeof(command),
                               MPU_TRANSFER_TIMEOUT_MS);
}

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(mpu.device, &reg, 1, data, length,
                                       MPU_TRANSFER_TIMEOUT_MS);
}

static int16_t signed_be16(const uint8_t *bytes)
{
    return (int16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static esp_err_t init_mpu(sensor_t *sensor)
{
    (void)sensor;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = MPU_I2C_PORT,
        .sda_io_num = MPU_I2C_SDA_GPIO,
        .scl_io_num = MPU_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &mpu.bus);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not create I2C bus on SDA GPIO%d/SCL GPIO%d: %s",
                 MPU_I2C_SDA_GPIO, MPU_I2C_SCL_GPIO,
                 esp_err_to_name(error));
        return error;
    }

    error = i2c_master_probe(mpu.bus, MPU_I2C_ADDRESS_AD0_LOW,
                             MPU_TRANSFER_TIMEOUT_MS);
    if (error == ESP_OK) {
        mpu.address = MPU_I2C_ADDRESS_AD0_LOW;
    } else {
        error = i2c_master_probe(mpu.bus, MPU_I2C_ADDRESS_AD0_HIGH,
                                 MPU_TRANSFER_TIMEOUT_MS);
        if (error != ESP_OK) {
            ESP_LOGE(TAG,
                     "No MPU acknowledged at 0x%02X or 0x%02X; check 3V3, "
                     "GND, SDA GPIO%d, SCL GPIO%d, CS high, and pull-ups",
                     MPU_I2C_ADDRESS_AD0_LOW, MPU_I2C_ADDRESS_AD0_HIGH,
                     MPU_I2C_SDA_GPIO, MPU_I2C_SCL_GPIO);
            return ESP_ERR_NOT_FOUND;
        }
        mpu.address = MPU_I2C_ADDRESS_AD0_HIGH;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = mpu.address,
        .scl_speed_hz = MPU_I2C_CLOCK_HZ,
    };
    error = i2c_master_bus_add_device(mpu.bus, &device_config, &mpu.device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not attach MPU at 0x%02X: %s", mpu.address,
                 esp_err_to_name(error));
        return error;
    }

    /* Reset, select the PLL clock, then configure 100 Hz, low-noise output. */
    error = write_register(MPU_REG_PWR_MGMT_1, 0x80);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "MPU reset command failed: %s", esp_err_to_name(error));
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if ((error = write_register(MPU_REG_PWR_MGMT_1, 0x01)) != ESP_OK ||
        (error = write_register(MPU_REG_CONFIG, 0x03)) != ESP_OK ||
        (error = write_register(MPU_REG_SMPLRT_DIV, 0x09)) != ESP_OK ||
        (error = write_register(MPU_REG_GYRO_CONFIG, 0x18)) != ESP_OK ||
        (error = write_register(MPU_REG_ACCEL_CONFIG, 0x10)) != ESP_OK ||
        (error = write_register(MPU_REG_ACCEL_CONFIG_2, 0x03)) != ESP_OK) {
        ESP_LOGE(TAG, "MPU configuration write failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    uint8_t identity = 0;
    error = read_registers(MPU_REG_WHO_AM_I, &identity, 1);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(error));
        return error;
    }
    if (identity != MPU_WHO_AM_I_6500 && identity != MPU_WHO_AM_I_9250) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I 0x%02X", identity);
        return ESP_ERR_NOT_FOUND;
    }
    mpu.initialized = true;
    ESP_LOGI(TAG, "Detected %s at I2C address 0x%02X",
             identity == MPU_WHO_AM_I_9250 ? "MPU-9250" : "MPU-6500",
             mpu.address);
    return ESP_OK;
}

static void start_mpu(sensor_t *sensor)
{
    (void)sensor;
    mpu.sample_status = ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < MPU_CHANNEL_COUNT; i++) {
        mpu.values[i] = 0;
    }
}

static esp_err_t read_mpu_channel(sensor_t *sensor, int32_t *value)
{
    mpu_sensor_context_t *context = sensor->context;

    /* The X-acceleration descriptor is first in the registry. Its burst read
     * updates the common snapshot consumed by the other five descriptors. */
    if (!mpu.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (context->channel == MPU_ACCEL_X) {
        uint8_t data[14];
        esp_err_t error = read_registers(MPU_REG_ACCEL_XOUT_H,
                                         data, sizeof(data));
        mpu.sample_status = error;
        if (error == ESP_OK) {
            /* +/-8 g: 4096 LSB/g. Report acceleration in milli-g. */
            mpu.values[MPU_ACCEL_X] = signed_be16(&data[0]) * 1000 / 4096;
            mpu.values[MPU_ACCEL_Y] = signed_be16(&data[2]) * 1000 / 4096;
            mpu.values[MPU_ACCEL_Z] = signed_be16(&data[4]) * 1000 / 4096;
            /* Skip temperature. +/-2000 dps: 16.4 LSB/(degree/s).
             * Report angular velocity in milli-degrees/second. */
            mpu.values[MPU_GYRO_X] = signed_be16(&data[8]) * 10000 / 164;
            mpu.values[MPU_GYRO_Y] = signed_be16(&data[10]) * 10000 / 164;
            mpu.values[MPU_GYRO_Z] = signed_be16(&data[12]) * 10000 / 164;
        } else {
            ESP_LOGW(TAG, "I2C sample failed: %s", esp_err_to_name(error));
        }
    }
    /* A failed burst invalidates every channel until the next good burst. */
    if (mpu.sample_status != ESP_OK) {
        return mpu.sample_status;
    }
    *value = mpu.values[context->channel];
    return ESP_OK;
}

static mpu_sensor_context_t contexts[MPU_CHANNEL_COUNT] = {
    {MPU_ACCEL_X}, {MPU_ACCEL_Y}, {MPU_ACCEL_Z},
    {MPU_GYRO_X}, {MPU_GYRO_Y}, {MPU_GYRO_Z},
};

#define MPU_SENSOR(descriptor_name, display_name, can_identifier, channel_id, \
                   init_callback, start_callback)                           \
    sensor_t descriptor_name = {                                           \
        .name = display_name,                                               \
        .can_id = can_identifier,                                           \
        .period_us = MPU_SAMPLE_PERIOD_US,                                  \
        .init = init_callback,                                              \
        .start = start_callback,                                            \
        .read = read_mpu_channel,                                           \
        .context = &contexts[channel_id],                                   \
    }

/* Only the first descriptor owns shared-device initialization and reset. */
MPU_SENSOR(mpu_accel_x_sensor, "mpu_accel_x", CAN_ID_MPU_ACCEL_X,
           MPU_ACCEL_X, init_mpu, start_mpu);
MPU_SENSOR(mpu_accel_y_sensor, "mpu_accel_y", CAN_ID_MPU_ACCEL_Y,
           MPU_ACCEL_Y, NULL, NULL);
MPU_SENSOR(mpu_accel_z_sensor, "mpu_accel_z", CAN_ID_MPU_ACCEL_Z,
           MPU_ACCEL_Z, NULL, NULL);
MPU_SENSOR(mpu_gyro_x_sensor, "mpu_gyro_x", CAN_ID_MPU_GYRO_X,
           MPU_GYRO_X, NULL, NULL);
MPU_SENSOR(mpu_gyro_y_sensor, "mpu_gyro_y", CAN_ID_MPU_GYRO_Y,
           MPU_GYRO_Y, NULL, NULL);
MPU_SENSOR(mpu_gyro_z_sensor, "mpu_gyro_z", CAN_ID_MPU_GYRO_Z,
           MPU_GYRO_Z, NULL, NULL);
