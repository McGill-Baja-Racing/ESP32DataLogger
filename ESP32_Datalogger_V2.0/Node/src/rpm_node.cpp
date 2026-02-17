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

#define SAMPLING_RATE               1000
#define BUFF_SIZE                   1024
#define READ_LEN                    256
#define VOLTAGE_RANGE               ADC_ATTEN_DB_12
#define MAPPING

#define TX_GPIO_NUM             21              // CAN TX Pin
#define RX_GPIO_NUM             20              // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth

#define TASK_CAN_MANAGER            "can_manager"
#define TASK_CAN_MAILBOX            "can_mailbox"
#define TASK_RPM_DISPATCHER         "rpm_dispatcher"
#define TASK_RPM_COLLECTOR          "rpm_collector"
TaskHandle_t process_rpm_signal = NULL;
TaskHandle_t flow_control = NULL;
/*
GUIDE
- https://controllerstech.com/esp32-9-how-to-use-adc-part2/#:~:text=channel%20contains%20the%20ADC%20channel,unit%20for%20the%20above%20channel.

TODO
1. Make sure ressource memory allocation makes sense
    .max_store_buf_size % .conv_frame_size = 0
2. Copy robotics github design --> for executing files
3. Add conv step from raw 

*/
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

static QueueHandle_t twai_rx_queue;
static QueueHandle_t twai_tx_queue;

static adc_channel_t channel[1] = {ADC_CHANNEL_1};
static const char *TAG = "ADC-DMA";

/* ------------- TIME OFFSET ------------ */
static volatile int64_t g_offset_us;
static inline void handle_time_beacon(const can_rx_word_t *rx){
    uint64_t t_main_us = rx ->data;
    int64_t t_local_us = esp_timer_get_time();

    g_offset_us = (int64_t)t_main_us - t_local_us;
}


static bool IRAM_ATTR signal_conv_done_callback(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notifying Conv Task to start converting
    vTaskNotifyGiveFromISR(xTaskGetHandle(TASK_RPM_DISPATCHER), &mustYield);
    return (mustYield == pdTRUE);
}

static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = BUFF_SIZE,
        .conv_frame_size = READ_LEN,
        .flags ={
            .flush_pool = 1,
        }
    };

    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    // Driver Configuration
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLING_RATE,
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
        .on_conv_done = signal_conv_done_callback, // .on_conv_done: means after one frane is collected
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &callbacks, NULL));
    *out_handle = handle;
}


static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8];
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
void can_mailbox_task(void *args){ // Find different name for task
    // will fetch info but also is responsible for program control flow
        // 1. Configure the node
    // 2. Install/Create the node
    static volatile int64_t g_offset_us;
    ESP_LOGI(TAG, "Driver installed");
    twai_node_handle_t node_hdl = NULL;

    twai_onchip_node_config_t node_config = {
        .io_cfg = {(gpio_num_t)TX_GPIO_NUM,(gpio_num_t)RX_GPIO_NUM},
        .bit_timing = { TRANSM_RATE},
        .tx_queue_depth = TX_QUEUE_DEPTH,
    };

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &callbacks, NULL)); 
    
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI node enabled");
    while (1){

    }
    ESP_ERROR_CHECK(twai_node_delete(node_hdl));

}

void can_manager_task(void *args){
    //vTaskDelay(5000); // Ensure that the handles are not Null;
    can_rx_word_t rx_msg;

    TaskHandle_t rpm_dispather_hdl = xTaskGetHandle(TASK_RPM_DISPATCHER);
    TaskHandle_t rpm_collector_hdl = xTaskGetHandle(TASK_RPM_COLLECTOR);

    while(1){
        xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);
        switch (rx_msg.id)
        {
        case ID_MASTER_TIME_BEACON:
            /* code */
            handle_time_beacon(&rx_msg);
            ESP_LOGI(TAG, "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32, rx_msg.id, (uint32_t)rx_msg.data);
            break;
        case ID_START_CMD:
            ESP_LOGI(TAG, "RECEIVED START MAIL");
            //configASSERT( rpm_dispather_hdl ); // check that task is not null
            vTaskResume( rpm_dispather_hdl );
            break;
        case ID_STOP_CMD:
            ESP_LOGI(TAG, "RECEIVED STOP MAIL");
            vTaskSuspend( rpm_dispather_hdl );
            xTaskNotifyGive(rpm_collector_hdl);
            break;
        default:
            ESP_LOGI(TAG, "MAIL TO NOBODY");
            break;
        }
    }

}

void rpm_dispatcher (void *args){
    while(1){
        
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Notified from callback function
        xTaskNotifyGive(xTaskGetHandle(TASK_RPM_COLLECTOR));
    }
}
void rpm_collector_task(void *args)
{
    //vTaskSuspend( NULL ); // Prevents task from running before command has been sent
    esp_err_t ret;  
    uint32_t ret_num = 0;
    uint8_t result[READ_LEN] = {0};
    memset(result, 0xcc, READ_LEN); // Writting an initial value: 0xcc is a know debugging practice

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
    uint64_t data = 0;
    bool peak_detected = false;
    while (1) {

        /**
         * This is to show you the way to use the ADC continuous mode driver event callback.
         * This `ulTaskNotifyTake` will block when the data processing in the task is fast.
         * However in this example, the data processing (print) is slow, so you barely block here.
         *
         * Without using this event callback (to notify this task), you can still just call
         * `adc_continuous_read()` here in a loop, with/without a certain block timeout.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Notified from callback function
        ESP_LOGI("TASK", "Fetching RPM Data");
        while (1) {

            ret = adc_continuous_read(handle, result, READ_LEN, &ret_num, 0);
            if (ret == ESP_OK) {
                //ESP_LOGI("TASK", "ret is %x, ret_num is %" PRIu32" bytes", ret, ret_num);

                adc_continuous_data_t parsed_data[ret_num / SOC_ADC_DIGI_RESULT_BYTES];
                uint32_t num_parsed_samples = 0;

                esp_err_t parse_ret = adc_continuous_parse_data(handle, result, ret_num, parsed_data, &num_parsed_samples);
                if (parse_ret == ESP_OK) {
                    for (int i = 0; i < num_parsed_samples; i++) {
                        if (parsed_data[i].valid) {
                            esp_err_t cali_ret = adc_cali_raw_to_voltage(calib_handle, parsed_data[i].raw_data, &processed);
                            //ESP_LOGI(TAG, "Reading data: %d", processed);

                            /* ------------ PEAK CATCHER ----------*/
                            if (processed < 1000 && !peak_detected) {

                                peak_detected = true; 
                                 // Capture the timestamp in microseconds
                                int64_t t_local_sample_us = esp_timer_get_time();
                                int64_t t_main_sample_us = t_local_sample_us + g_offset_us;

                                // Optionally, you can keep timestamp in microseconds for better precision
                                int32_t timestamp = (int32_t)(t_main_sample_us / 1000); // Convert from us to ms
                                int32_t value = (int32_t)processed;

                                // Pack timestamp and value into the data structure
                                data =((uint64_t)(uint32_t)timestamp << 32) | ((uint64_t)(uint32_t)value );
                                ESP_LOGI(TAG, "raw  data: %d", processed);


                            }else if (processed > 1000 && peak_detected) {
                                peak_detected = false;
                            }

                        } else {
                            ESP_LOGW(TAG, "Invalid data [ADC%d_Ch%d_%" PRIu32"]",
                                     parsed_data[i].unit + 1,
                                     parsed_data[i].channel,
                                     parsed_data[i].raw_data);
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Data parsing failed: %s", esp_err_to_name(parse_ret));
                }

                /**
                 * Because printing is slow, so every time you call `ulTaskNotifyTake`, it will immediately return.
                 * To avoid a task watchdog timeout, add a delay here. When you replace the way you process the data,
                 * usually you don't need this delay (as this task will block for a while).
                 */
                vTaskDelay(1);
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

static void send_mail_task(void *arg)
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

void rpm_node_main(){
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));

    xTaskCreate(can_mailbox_task,TASK_CAN_MAILBOX, 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(send_mail_task,"send_mail", 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(rpm_dispatcher,TASK_RPM_DISPATCHER, 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(rpm_collector_task,TASK_RPM_COLLECTOR, 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(can_manager_task,TASK_CAN_MANAGER, 4096, NULL,tskIDLE_PRIORITY,NULL);

}  

#endif
 