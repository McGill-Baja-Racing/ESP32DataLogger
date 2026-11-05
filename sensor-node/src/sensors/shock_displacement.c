#include "sensor.h"

#include "adc_input.h"
#include "protocol/app_protocol.h"

// Shock displacement in parallel
#define LIN_POT_1_GPIO                      0
#define LIN_POT_2_GPIO                      1
#define DIVIDER_SCALE                       1.0f
#define SENSOR_MIN_MV                       0.0f
#define SENSOR_MAX_MV                       3000.0f
#define MAX_RESISTANCE_kOhm                 10
#define TRAVEL_RANGE_SPAN_mm                100
#define SAMPLING_PERIOD_US                  10000


// If 5V input voltage
// Max ADC voltage around 3.1V
// 10kOhm/(Req) * 5V =~ 3.1V 
// Thus ~3/5 -> 10kOhm/0.6 = Req -> Req = 16.67 kOhm therefore min add 7kOhmn

// If 3V input voltage --> might need to add voltage divider or see what max value is


static esp_err_t read_lin_pot_displacement(sensor_t *sensor, int32_t *value)
{
    (void)sensor;
    int voltage_mv = 0;
    int channel = (int)(sensor->context);
    esp_err_t error = adc_input_read_mv(channel, &voltage_mv);
    
    if (error != ESP_OK) {
        return error;
    }
    
    float displacement_mm = ((voltage_mv - SENSOR_MIN_MV) / (SENSOR_MAX_MV- SENSOR_MIN_MV)) * TRAVEL_RANGE_SPAN_mm;
    *value = displacement_mm;
    return ESP_OK;
}

static esp_err_t init_shock_sensor(sensor_t *sensor){
    return adc_input_init();
}

#define SHOCK_SENSOR(descriptor_name, display_name, can_identifier, channel_id)\
    sensor_t descriptor_name = {                                             \
        .name = display_name,                                                \
        .can_id = can_identifier,                                            \
        .period_us = SAMPLING_PERIOD_US,                                     \
        .init = init_shock_sensor,                                           \
        .read = read_lin_pot_displacement,                                   \
        .context = (void*)channel_id                                \
    }          

#if NODE_FIXED_FRONT_SHOCK_CONFIG
SHOCK_SENSOR(lin_pot_1_sensor,"front_lin_pot_1",CAN_ID_FRONT_LIN_1,LIN_POT_1_GPIO);
SHOCK_SENSOR(lin_pot_2_sensor,"front_lin_pot_2",CAN_ID_FRONT_LIN_2,LIN_POT_2_GPIO);
#elif NODE_FIXED_REAR_SHOCK_CONFIG
SHOCK_SENSOR(lin_pot_1_sensor,"rear_lin_pot_1",CAN_ID_REAR_LIN_1,LIN_POT_1_GPIO);
SHOCK_SENSOR(lin_pot_2_sensor,"rear_lin_pot_2",CAN_ID_REAR_LIN_2,LIN_POT_2_GPIO);
#endif



