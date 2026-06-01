#include "adc_oneshot_node.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"


#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "../lib/can/frames.c"
#include "time_sync.hpp"
#include "sensor_specific_code.hpp"


const static char *TAG = "ONESHOT";


/*---------------------------------------------------------------
        ADC General Macros
---------------------------------------------------------------*/

//ADC1 Channels
#define ADC1_CHAN0          ADC_CHANNEL_3 // GPIO PIN (e.g. sensor pin)
#define ADC_UNIT            ADC_UNIT_1 // ADC chip
#define ADC_ATTEN           ADC_ATTEN_DB_12 // Need 12 DB attentuation to maximise voltage input range. Now it is between  150 ∼ 2450 mV 
#define ADC_RES             ADC_BITWIDTH_DEFAULT // Measurement resolution


static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
static void adc_calibration_deinit(adc_cali_handle_t handle);

adc_oneshot_unit_handle_t adc1_handle;
adc_cali_handle_t adc1_cali_chan0_handle = NULL;

/*---------------------------------------------------------------
        ADC Init and Config
---------------------------------------------------------------*/
void adc_oneshot_init(){
   adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_RES, // measurement resolution
    };
       
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC1_CHAN0, &config));

}

/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }
    return calibrated;
}

static void adc_calibration_deinit(adc_cali_handle_t handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    //ESP_LOGI(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "deregister %s calibration scheme", "Line Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));
#endif
}

bool adc_oneshot_and_calib_init(){ // output is true if calib is sucessfull
    adc_oneshot_init();
    return adc_calibration_init(ADC_UNIT_1, ADC1_CHAN0, ADC_ATTEN, &adc1_cali_chan0_handle);
}

void adc_oneshot_and_calib_deinit(bool calib_performed){
    if (calib_performed) {
        ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
        adc_calibration_deinit(adc1_cali_chan0_handle);
    }
}

void adc_oneshot_main(bool *loop_condition,bool do_calibration1_chan0,QueueHandle_t queue_hdl)
{
    // Make sure to do deinit after calibration or else will get an error
    //-------------ADC1 Init---------------//
    int adc_raw;
    int adc_raw_voltage;
    uint64_t adc_mapped_data;
    uint64_t packaged_data;

    bool mapping_succeeded = false;

    while (*loop_condition) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC1_CHAN0, &adc_raw));
        ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, ADC1_CHAN0, adc_raw);
        if (do_calibration1_chan0) {
            // READ: https://documentation.espressif.com/esp32_datasheet_en.pdf
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle, adc_raw, &adc_raw_voltage));
            mapping_succeeded = sensor_specific_data_mapping(adc_raw_voltage,&adc_mapped_data);
            if (mapping_succeeded){
                packaged_data = package_timestamp_with_data(adc_mapped_data); // IMP: SEE WITH REMY FOR THI
                
                xQueueSend(queue_hdl,&packaged_data, 0); // HANDLE CASE IF QUEUE IS FULL!!
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));

    }
}

