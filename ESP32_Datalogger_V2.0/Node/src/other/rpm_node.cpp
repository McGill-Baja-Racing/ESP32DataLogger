#ifdef RPM_NODE
#include "rpm_node.hpp"
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

#define ADC_SAMPLING_FREQ           1000 // of measurement --> need measurements every ms therefore can maybe sample 3000 hz get 3 values and average it
#define BUFF_SIZE                   1024
//#define READ_LEN                    256
uint32_t measurements_per_ms = ADC_SAMPLING_FREQ/1000;
uint32_t READ_LEN = measurements_per_ms *1* SOC_ADC_DIGI_DATA_BYTES_PER_CONV;

#define VOLTAGE_RANGE               ADC_ATTEN_DB_12
#define MAPPING

#define TX_GPIO_NUM             21              // CAN TX Pin
#define RX_GPIO_NUM             20              // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_LENGTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
TaskHandle_t can_mailbox_task_hdl = NULL;
TaskHandle_t send_mail_task_hdl = NULL;
TaskHandle_t rpm_dispatcher_task_hdl = NULL;
TaskHandle_t rpm_collector_task_hdl = NULL;

twai_node_handle_t node_hdl = NULL;
static QueueHandle_t twai_rx_queue;
static QueueHandle_t twai_tx_queue;


bool ignore_button = true;
/*
GUIDE
- https://controllerstech.com/esp32-9-how-to-use-adc-part2/#:~:text=channel%20contains%20the%20ADC%20channel,unit%20for%20the%20above%20channel.

TODO
- 1) Send data rpm data to master
- ADDRESS ISSUES -> ISSUES
- Address Improvements: I

*/
static const char *TAG = "ADC-DMA";

static esp_err_t adc_sampling_validation_for_1ms_rpm_tx_frequency(uint32_t f_Hz){
    /* Checks the spark plug sampling.
       Checks that sampling freq has ms resolution:
       Let f_Hz = sampling_freq in [Hz]
       Minimum requirement for 1ms resolution: f_Hz >= 1000 Hz
    */

    if (f_Hz < 1000) {
        ESP_LOGI(TAG, "RPM 1ms transmission frequency is not satisfied. ADC Sampling frequency is too low (%d Hz). Must be >= 1000 Hz", f_Hz);
        return ESP_ERR_INVALID_ARG;
    } else {
        ESP_LOGI(TAG, "RPM 1ms transmission frequency of RPM signal is satisfied (%d Hz).", f_Hz);
        return ESP_OK;
    };
}

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

typedef struct{
    uint32_t timestamp;
    uint32_t data;
} data_instance;



static adc_channel_t channel[1] = {ADC_CHANNEL_1}; // Pin number 1

/* ------------- TIME OFFSET ------------ */
static volatile int32_t g_offset_us;
static inline void handle_time_beacon(const can_rx_word_t *rx){
    uint64_t t_main_us = rx ->data;
    int64_t t_local_us = esp_timer_get_time();
    g_offset_us = (int32_t)((int64_t)t_main_us - t_local_us);
}
// ISSUES: Crititcal Section --> g_offset is 64 bit but processor is 32 bit (not written in a single instruction)

static bool IRAM_ATTR adc_conv_done_callback(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    // Awakens task responsible of processing the data
    //Notifying Conv Task to start converting
    vTaskNotifyGiveFromISR(rpm_dispatcher_task_hdl, &mustYield);
    return (mustYield == pdTRUE);
}

static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle)
{   // The 3 lines below ensure that every 1ms raw data values will be parsed.
    ESP_ERROR_CHECK(adc_sampling_validation_for_1ms_rpm_tx_frequency(ADC_SAMPLING_FREQ));

    
    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = BUFF_SIZE,
        .conv_frame_size = READ_LEN,
        .flags ={
            .flush_pool = 1,
        }
    };

    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle)); 
    /* IMPORTANT -- if error is thrown here there are two reasons:
        1) BUFF_SIZE < frame_size
        2) frame_size is not a multiple of BUFF_SIZE
    */

    // Driver Configuration
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = ADC_SAMPLING_FREQ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    // Multichannel configation
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        adc_pattern[i].atten = ADC_ATTEN_DB_12;
        adc_pattern[i].channel = channel[i] & 0x7;
        adc_pattern[i].unit = ADC_UNIT_1;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%" PRIx8, i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%" PRIx8, i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%" PRIx8, i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));
    
    // Event callback
    adc_continuous_evt_cbs_t callbacks = {
        .on_conv_done = adc_conv_done_callback, // .on_conv_done: means after one frane is collected
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &callbacks, NULL));
    *out_handle = handle;
}

static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8]; // 64 bits
    twai_frame_t frame = {
        .buffer     = raw,
        .buffer_len = sizeof(raw),
    };

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {

        can_rx_word_t msg;
        msg.id  = frame.header.id;
        msg.dlc = frame.header.dlc;
        msg.data = 0;

        // Pack into uint64_t (little-endian)
        for (int i = 0; i < msg.dlc; i++) {
            msg.data |= ((uint64_t)raw[i] << (8 * i));
        }

        xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }
    return hp_woken == pdTRUE;
}


twai_event_callbacks_t callbacks = {
.on_rx_done = twai_rx_cb,
};

/*------------ TASKS -------------*/
void can_manager_task(void *args){ // Find different name for task
    // will fetch info but also is responsible for program control flow
        // 1. Configure the node
    // 2. Install/Create the node
    ESP_LOGI(TAG, "Driver installed");

    twai_onchip_node_config_t node_config = {
        .io_cfg = {(gpio_num_t)TX_GPIO_NUM,(gpio_num_t)RX_GPIO_NUM},
        .bit_timing = { TRANSM_RATE},
        .tx_queue_depth = TX_QUEUE_LENGTH,
    };

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &callbacks, NULL)); 
    
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI node enabled");

    can_rx_word_t rx_msg;

    if (ignore_button){
        ESP_LOGI(TAG, "Running Script without start-stop control");
        ESP_LOGI(TAG, "STARTING DISPATCHER - Should be Reading and Sending RPM data");
                //configASSERT( rpm_dispather_hdl ); // check that task is not null
        vTaskResume( rpm_dispatcher_task_hdl );
        while (1){

        }
    }else{
        ESP_LOGI(TAG, "Running Script with start-stop control");

        while(1){
            xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);
            switch (rx_msg.id)
            {
            case ID_MASTER_TIME_BEACON:
                handle_time_beacon(&rx_msg);
                ESP_LOGI(TAG, "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32, rx_msg.id, (uint32_t)rx_msg.data);
                break;
            case ID_START_CMD:
                ESP_LOGI(TAG, "RECEIVED START MAIL");
                //configASSERT( rpm_dispather_hdl ); // check that task is not null
                vTaskResume( rpm_dispatcher_task_hdl );
                break;
            case ID_STOP_CMD:
                ESP_LOGI(TAG, "RECEIVED STOP MAIL");
                vTaskSuspend( rpm_dispatcher_task_hdl );
                xTaskNotifyGive(rpm_collector_task_hdl);
                break;
            default:
                ESP_LOGI(TAG, "MAIL TO NOBODY");
                break;
            }
        }
    }


    ESP_ERROR_CHECK(twai_node_delete(node_hdl));

}
/*
void can_manager_task(void *args){
    //FUSE THIS INTO CAN MAILBOX_TASK
    can_rx_word_t rx_msg;

    while(1){
        xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);
        switch (rx_msg.id)
        {
        case ID_MASTER_TIME_BEACON:
            handle_time_beacon(&rx_msg);
            ESP_LOGI(TAG, "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32, rx_msg.id, (uint32_t)rx_msg.data);
            break;
        case ID_START_CMD:
            ESP_LOGI(TAG, "RECEIVED START MAIL");
            //configASSERT( rpm_dispather_hdl ); // check that task is not null
            vTaskResume( rpm_dispatcher_task_hdl );
            break;
        case ID_STOP_CMD:
            ESP_LOGI(TAG, "RECEIVED STOP MAIL");
            vTaskSuspend( rpm_dispatcher_task_hdl );
            xTaskNotifyGive(rpm_collector_task_hdl);
            break;
        default:
            ESP_LOGI(TAG, "MAIL TO NOBODY");
            break;
        }
    }

}*/

void rpm_dispatcher (void *args){ // Intermediate Task to Notify Collector Task to Collect RPM Data
    while(1){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Notified from callback function
        xTaskNotifyGive(rpm_collector_task_hdl);
    }
}

static int64_t get_data_timestamp_in_ms(){
    int64_t t_main_sample_us = esp_timer_get_time() + g_offset_us;
    return (int32_t)(t_main_sample_us / 1000); // Convert from us to ms
};

static int64_t get_data_timestamp_in_us(){
    int64_t t_main_sample_us = esp_timer_get_time() + g_offset_us;
    return (int32_t)(t_main_sample_us); // Convert from us to ms
};

void rpm_collector_task(void *args)
{
    //vTaskSuspend( NULL ); // Prevents task from running before command has been sent
    esp_err_t ret;  
    uint32_t ret_num = 0;
    uint8_t raw_data[READ_LEN] = {0};
    i
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

    int processed = 0;
    bool first_peak_detected = false;
    uint64_t prev_peak_ts = 0;
    uint64_t curr_peak_ts = 0;
    float_t peak_interval = 0;

    uint64_t data = 0;
    bool peak_detected = false;
    uint32_t peak_count;
    uint32_t num_valid_parsed_samples;
    float_t rpm_estimate;
    ESP_LOGI("TASK", "PARSING RPM Data");

    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Notified from callback function
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
                    curr_peak_ts = 0;
                    
                    for (int i = 0; i < num_parsed_samples; i++) {
                        if (parsed_data[i].valid) {
                            num_valid_parsed_samples +=1;
                            esp_err_t cali_ret = adc_cali_raw_to_voltage(calib_handle, parsed_data[i].raw_data, &processed);
                            //ESP_LOGI(TAG, "Reading data: %d", processed);

                            /* ------------ PEAK CATCHER ----------*/
                            static int last_processed = 0;
                            
                            if (!peak_detected && last_processed < 100 && processed >= 1900) {
                                peak_detected = true;
                                curr_peak_ts = get_data_timestamp_in_us();
                            } else if (peak_detected && processed < 1900) {
                                peak_detected = false;
                            }
                            last_processed = processed;

                        } else {
                            ESP_LOGW(TAG, "Invalid data [ADC%d_Ch%d_%" PRIu32"]",
                                     parsed_data[i].unit + 1,
                                     parsed_data[i].channel,
                                     parsed_data[i].raw_data);
                        }
                    } 
                    
                    if (num_valid_parsed_samples > 0){
                        if (curr_peak_ts!=0){
                            if (!first_peak_detected){
                                prev_peak_ts = curr_peak_ts;
                                first_peak_detected = true;
                            }else{
                                peak_interval = (curr_peak_ts - prev_peak_ts);
                                prev_peak_ts = curr_peak_ts;
                                rpm_estimate = 6*(1.0f/(2.0f*peak_interval/1000.0f));//30000 since peak_interval is actually half a period
                                ESP_LOGI(TAG, "rpm estimate: %f, peak_interval: %f, num %d", rpm_estimate, peak_interval);
                            }

                        }else{
                            ESP_LOGI(TAG, "None");

                        }
                        
                        /*
                        rpm_estimate = (peak_count * ADC_SAMPLING_FREQ * 60)/num_valid_parsed_samples;
                        data = ((uint64_t)(uint32_t)timestamp << 32) | (rpm_estimate);
                        xQueueSend(twai_tx_queue, &data, 0);
                        ESP_LOGI(TAG, "raw  data: %d, rpm estimate: %d", processed, rpm_estimate);*/
                    }else{
                        ESP_LOGW(TAG,"All %d data sampled parsed were invalid. Consider Investigating", num_parsed_samples);
                    }
                    
                    /* ------------------ NEXT -------------------*/
                    // Send DATA To Master
                    /* -------------------------------------------*/

                } else {
                    ESP_LOGE(TAG, "Data parsing failed: %s", esp_err_to_name(parse_ret));
                }

                vTaskDelay(1); // I: Is this necessary?
            } else if (ret == ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "ADC NO available data: %s", esp_err_to_name(ret));

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

static void send_mail_to_node_task(void *arg)
{
    /*
    SIMULATES SENDING CAN FRAMES --> FOR SOME REASON THE TIMING / DELAY IS OFF
    */
    bool message_type = false;
    can_rx_word_t msg;

    while (1)
    {   

        message_type=!message_type;
        if (message_type){
            msg.id = ID_START_CMD;
            ESP_LOGI(TAG, "START");
        }else{
            msg.id = ID_STOP_CMD;
            ESP_LOGI(TAG, "STOP");

        }
        xQueueSend(twai_rx_queue, &msg, 0);

        vTaskDelay(1000);       
    }
}

static void send_rpm_data_to_master(void *arg){
    int64_t data;

    while(1){
        xQueueReceive(twai_tx_queue,&data,portMAX_DELAY);
        ESP_LOGI(TAG, "Sending rpm data to master");
        memcpy(rpm_msg_data, &data, sizeof(rpm_msg_data)); // assigning value to byte array
        ESP_ERROR_CHECK(twai_node_transmit(node_hdl,&rpm_message,0)); // sending data 
    }
}

void rpm_node_main(){
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    twai_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(can_rx_word_t));

    //xTaskCreate(can_mailbox_task,"can_mailbox", 4096, NULL,tskIDLE_PRIORITY,&can_mailbox_task_hdl);
    //xTaskCreate(send_mail_to_node_task,"send_mail", 4096, NULL,tskIDLE_PRIORITY,&send_mail_task_hdl);
    xTaskCreate(rpm_dispatcher,"rpm_dispatcher", 4096, NULL,tskIDLE_PRIORITY,&rpm_dispatcher_task_hdl);
    xTaskCreate(rpm_collector_task,"rpm_collector", 4096, NULL,tskIDLE_PRIORITY,&rpm_collector_task_hdl);
    xTaskCreate(can_manager_task,"can_manager", 4096, NULL,tskIDLE_PRIORITY,NULL);
    //xTaskCreate(send_rpm_data_to_master,"send_data_to_master", 4096, NULL,tskIDLE_PRIORITY,NULL);
}  

#endif
 /*
 1. Break down code on paper
 1. Create Documentation --> Make a diagram to explain interactions
 2. Figure out how to add the transmit of the peaks to the P4
 3. 
 
 */