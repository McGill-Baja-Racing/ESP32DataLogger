#include "sensor.h"
#include "adc_input.h"
#include "protocol/app_protocol.h"
#include <math.h>

// --- Hardware Definitions ---
#define THERMISTOR_GPIO         3
#define THERMOPILE_GPIO         2

// --- System & Circuit Constants ---
#define V_IN_MV                 3300.0f  // 3.3V supply for voltage divider
#define R_FIXED                 100000.0f
#define AMP_GAIN                247.0f   // Gain from INA126[cite: 1]
#define SENSITIVITY_MV_C        0.11f    // ZTP-148SR sensitivity[cite: 1]

// --- Thermistor Constants ---
#define R0                      100000.0f
#define T0                      298.15f
#define BETA                    3960.0f

static int32_t read_cvt_temperature(sensor_t *sensor)
{
    (void)sensor;
    int thermistor_mv = 0;
    int thermopile_mv = 0;

    // Read both ADC channels in millivolts
    if (adc_input_read_mv(THERMISTOR_GPIO, &thermistor_mv) != ESP_OK ||
        adc_input_read_mv(THERMOPILE_GPIO, &thermopile_mv) != ESP_OK) {
        return 0;
    }

    // Prevent division by zero if pin is shorted to ground
    if (thermistor_mv <= 0) {
        return 0;
    }

    // 1. Calculate Ambient Temperature (Thermistor)
    float r_therm = R_FIXED * ((V_IN_MV / (float)thermistor_mv) - 1.0f);
    
    // Prevent log of negative or zero resistance (hardware fault)
    if (r_therm <= 0.0f) {
        return 0;
    }
    
    float temp_kelvin = 1.0f / ((1.0f / T0) + (1.0f / BETA) * logf(r_therm / R0));
    float ambient_temp_c = temp_kelvin - 273.15f;

    // 2. Calculate Target Temperature (Thermopile)
    // Reverse the amplifier gain to get the true sensor signal[cite: 1]
    float true_thermopile_mv = (float)thermopile_mv / AMP_GAIN;
    
    // Calculate the temperature difference[cite: 1]
    float delta_t = true_thermopile_mv / SENSITIVITY_MV_C;
    
    // Add cold junction compensation (ambient temp)
    float target_temp_c = ambient_temp_c + delta_t;

    // Return as a standard integer (e.g., 85 degrees)
    // If your CAN protocol requires higher precision, you can scale this 
    // before casting: return (int32_t)(target_temp_c * 10.0f); 
    return target_temp_c > 0.0f ? (int32_t)target_temp_c : 0;
}

sensor_t cvt_belt_sensor = {
    .name = "cvt_belt_temperature",
    .can_id = CAN_ID_CVT_TEMP,  // Ensure this is defined in app_protocol.h
    .period_us = 100000,        // 100ms (10 Hz update rate)
    .read = read_cvt_temperature,
};