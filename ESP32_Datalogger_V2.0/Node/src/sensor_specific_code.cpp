
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "time_sync.hpp"
#include "sensor_specific_code.hpp"



#define ADC_MAX_VOLTAGE_mV  2450.0f   // Use for standard ESP32

static const char *TAG = "sensor_map";

bool sensor_specific_data_mapping(int voltage, uint64_t *out_value){

#if defined(NODE_ID) && NODE_ID == 1 // Pressure Sensor

    constexpr float SENSOR_MIN_VOLTAGE_mV = 500.0f;
    constexpr float SENSOR_MAX_VOLTAGE_mV     = 1600.0f;

    float min_voltage_value = SENSOR_MIN_VOLTAGE_mV * ADC_MAX_VOLTAGE_mV/SENSOR_MAX_VOLTAGE_mV; // ADJUSTED 
    if (voltage< min_voltage_value){ // need to adjust the 500 mV due to the resistors
        ESP_LOGE(TAG, "Invalid data. Value should be greater than %f mv since %f mv is 0 psi",min_voltage_value);
        return false;
    }else{
        float pressure_span = 1600.0f;
        float voltage_span = ADC_MAX_VOLTAGE_mV - min_voltage_value;
        float pressure =((float)(voltage - min_voltage_value) / voltage_span)* pressure_span;
        ESP_LOGI(TAG, "Pressure value %f psi",pressure);
        *out_value = package_timestamp_with_data((int64_t)pressure);
        return true;
    }

#elif defined(NODE_ID) && NODE_ID ==2


#elif defined(NODE_ID) && NODE_ID ==3


#endif

}
