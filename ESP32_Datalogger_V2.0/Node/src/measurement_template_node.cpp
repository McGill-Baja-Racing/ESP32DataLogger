#ifdef RANDOM_NODE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"

// ADC Imports
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h" 
#include "esp_adc/adc_cali_scheme.h"

// TWAI
#include "esp_twai.h"
#include "esp_twai_onchip.h"

// Frames
#include "../lib/can/frames.c"

// TIMER
#include "esp_timer.h"

// C++
#include <algorithm>
#include <vector>
#include <math.h>

void collect_data(void *args)
{
    //vTaskSuspend( NULL ); // Prevents task from running before command has been sent
    esp_err_t ret;  
    uint32_t ret_num = 0;
    uint8_t raw_data[READ_LEN] = {0};
    
    memset(raw_data, 0xcc, READ_LEN); // Number of bytes to be set to a value. Writting an initial value: 0xcc is a know debugging practice

    // Initializing Continuous ADC
    adc_continuous_handle_t handle = NULL;
    adc_cali_handle_t calib_handle = NULL;

    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &handle);
        adc_cali_curve_fitting_config_t cali_config ={
        .unit_id = ADC_UNIT_1,
        .atten = VOLTAGE_RANGE,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &calib_handle));

    ESP_ERROR_CHECK(adc_continuous_start(handle)); // Analog to digital Converter

    float measured_value = 0;
    uint64_t data = 0;
    bool peak_detected = false;
    uint32_t peak_count;
    uint32_t num_valid_parsed_samples;

    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Notified from callback function
        ESP_LOGI("TASK", "PARSING RPM Data");
        int32_t timestamp = get_data_timestamp_in_ms(); // Get timestamp when data just finished being recorded

        while (1) {
            ret = adc_continuous_read(handle, raw_data, READ_LEN, &ret_num, 0);
            if (ret == ESP_OK) {
                //ESP_LOGI("TASK", "ret is %x, ret_num is %" PRIu32" bytes", ret, ret_num);

                adc_continuous_data_t parsed_data[ret_num / SOC_ADC_DIGI_RESULT_BYTES]; // ISSUE: Apparently this is not safe
                uint32_t num_parsed_samples = 0;

                esp_err_t parse_ret = adc_continuous_parse_data(handle, raw_data, ret_num, parsed_data, &num_parsed_samples);

                if (parse_ret == ESP_OK) { 
                    peak_count = 0;
                    num_valid_parsed_samples = 0;
                    for (int i = 0; i < num_parsed_samples; i++) {
                        if (parsed_data[i].valid) {
                            num_valid_parsed_samples +=1;
                            esp_err_t cali_ret = adc_cali_raw_to_voltage(calib_handle, parsed_data[i].raw_data, &measured_value);
                            //ESP_LOGI(TAG, "Reading data: %d", processed);

                        } else {
                            ESP_LOGW(TAG, "Invalid data [ADC%d_Ch%d_%" PRIu32"]",
                                     parsed_data[i].unit + 1,
                                     parsed_data[i].channel,
                                     parsed_data[i].raw_data);
                        }
                    } 
                    ESP_LOGI(TAG, "pressure: %.2f", measured_value);

                vTaskDelay(1); // I: Is this necessary?
            } else if (ret == ESP_ERR_TIMEOUT) {
                //We try to read `EXAMPLE_READ_LEN` until API returns timeout, which means there's no available data
                break;
            }
        }
        ESP_LOGI("TASK", "Finished Processing RPM Data");

    }
    ESP_ERROR_CHECK(adc_continuous_stop(handle));
    ESP_ERROR_CHECK(adc_continuous_deinit(handle));
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(calib_handle));
}
}
void main(){
    xTaskCreate(collect_data,"rpm_collector", 4096, NULL,tskIDLE_PRIORITY);
}  
#endif