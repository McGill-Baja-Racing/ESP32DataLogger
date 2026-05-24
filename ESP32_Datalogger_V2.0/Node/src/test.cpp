#ifdef SENSOR_TEMPLATE
#include "test.hpp" // TO CHNAGE

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"


#include "../lib/can/frames.c"
#include "adc_oneshot_node.hpp"



// Defining Stringify to have the MACRO in the TAG
#define STR_HELPER(x) #x
#define STRINGIFY(x) STR_HELPER(x)
static const char *TAG = "NODE_" STRINGIFY(NODE_ID);
//static const char *TAG = "Node";
/* --------------------- Logging / buffer config ------------------ */
#define SAMPLE_SIZE_INT64  2
#define SAMPLES_PER_BLOCK  100
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE_INT64 * sizeof(int64_t))
#define MAX_BLOCK_WRITES   2
#define LOG_EVERY_N        50

#define ID_MASTER_TIME_BEACON   0x0A2
#define TIME_BEACON_PERIOD_MS   100   



/* --------------------- TWAI config ------------------ */
#define TX_GPIO_NUM             5
#define RX_GPIO_NUM             4
#define TRANSM_RATE             1000000
#define TX_QUEUE_LENGTH          5             
#define RX_QUEUE_LENGTH         256

/* ---------------------- TWAI CONFIG -------------------- */
static twai_node_handle_t node_hdl = NULL;
static twai_onchip_node_config_t node_config = {
    .io_cfg = {
        .tx = (gpio_num_t)TX_GPIO_NUM,
        .rx = (gpio_num_t)RX_GPIO_NUM,
    },
    .bit_timing = {
        .bitrate = TRANSM_RATE,
    },
    .tx_queue_depth = TX_QUEUE_LENGTH,
};


static QueueHandle_t twai_tx_queue = NULL;
static QueueHandle_t twai_rx_queue = NULL;


TaskHandle_t daq_task_hdl = NULL;
TaskHandle_t reboot_task_hdl = NULL;


bool is_collecting_data = false;

static bool twai_rx_cb(twai_node_handle_t node_hdl, const twai_rx_done_event_data_t *edata,void *user_ctx)
{
    // Handles Commands From P4
    BaseType_t hp_task_woken = pdFALSE;

    uint8_t recv_buff[8];
    twai_frame_t frame = {
        .buffer     = recv_buff,
        .buffer_len = sizeof(recv_buff),
    };
    if (ESP_OK == twai_node_receive_from_isr(node_hdl, &frame)){
        uint8_t dlc = frame.header.dlc;
        uint64_t packed = 0;

        if (dlc > 8) dlc = 8; // CAN frames should never exceed 8 bits
        for (int i = 0; i <dlc; i++) {
            packed |= (uint64_t)recv_buff[i] << (8 * i); // little-endian packing
        }

         can_rx_word_t msg = { // why?? to transfor into a message to send data to a task?
            .id = frame.header.id,
            .dlc = dlc,
            .data = packed
         };

        (void)xQueueSendFromISR(twai_rx_queue, &msg, &hp_task_woken); // send info to process_master_cmds_task
        /* flags task for context switiching if it was waiting on a msg since the queue was empty*/
        if (hp_task_woken) {
            portYIELD_FROM_ISR(); // Performs context switch but waits to exit twai_rx_cb first
        }
    }
    return hp_task_woken == pdTRUE; // if hp_task_woken: higher priority task has woken up
}

static twai_event_callbacks_t cbs = { // function to call when data is received
    .on_rx_done = twai_rx_cb,
};

/* --------------------- Button Control ------------------ */

/* ------------------- Start and Stop ---------------------- */


void process_master_cmds_task(void *args){
    
    can_rx_word_t rx_msg;

    while (1){
        xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);

        // ADD FILTER FOR ID_REBOOT -> so can also target specific nodes
        if (NODE_ID != (rx_msg.id & CAN_NODE_MASK)){ // Message is not addressed to current sensor
            ESP_LOGI(TAG, "Not for Node");
            ESP_LOGI(TAG, "ID: 0x%03" PRIx32, rx_msg.id);
            continue;
        }
        ESP_LOGI(TAG, "Executing command");
        ESP_LOGI(TAG, "ID: 0x%03" PRIx32, rx_msg.id);

        ESP_LOGI(TAG, "ID: 0x%03" PRIx32, (rx_msg.id & CAN_CMD_MASK));
        switch (rx_msg.id & CAN_CMD_MASK) // Check command
        {
        case ID_REBOOT_CMD:
            /* code */
            xTaskNotifyGive(reboot_task_hdl);
            break;
        case ID_START_CMD:
            /* enable */
            is_collecting_data = true;
            xTaskNotifyGive(daq_task_hdl);
            break;
        case ID_STOP_CMD:
            is_collecting_data = false; // because of while loop --> 
            /* code */
            break;
        default:
            ESP_LOGI(TAG, "This commands is not for node");
            break;
        }
    }

}

/* ----------------------- TASKS ------------*/
void reboot_task (void *args){
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "Reboot requested");
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_restart();
    }
}

void daq_task (void *args){
    ESP_LOGI(TAG, "Running");
    bool calib_performed = adc_oneshot_and_calib_init();
    while(1){

        if(!is_collecting_data){
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        adc_oneshot_main(&is_collecting_data,calib_performed);
        /*
            ADC ONESHOT MAIN COULD GO HERE
            - Depending on flag the mapping/calculations change (use ifdef)
        */

    }
    adc_oneshot_and_calib_deinit(calib_performed);
}

void v_send_control_cmd(void *args ){
    bool status = false;
    while(1){
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
            if (status){
                ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &start_message, 0));  // Timeout = 0: returns immediately if queue is full
            }else{
                ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &stop_message, 0));  // Timeout = 0: returns immediately if queue is full
            }
            status =! status;
            ESP_LOGI("btn-Task", "ISR Received");
            ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));  // Wait for transmission to finish
        }
    }
}
void send_data_to_master_task(void *args){
    while (1){
        ESP_LOGI(TAG, "Alive");

        vTaskDelay(1000);       
    }
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
            msg.id = 0xC11;
            ESP_LOGI(TAG, "START");
        }else{
            msg.id = 0xC21;
            ESP_LOGI(TAG, "STOP");

        }
        xQueueSend(twai_rx_queue, &msg, 0);

        vTaskDelay(1000);       
    }
}

/* --------------------- app_main ------------------ */

void node_main(){
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    twai_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(can_rx_word_t));

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    //ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    esp_err_t ret = twai_node_enable(node_hdl);
    ESP_LOGI(TAG, "twai_node_enable returned: %s", esp_err_to_name(ret));    ESP_LOGI(TAG, "TWAI enabled");

    ESP_LOGI(TAG, "Data logger started.");

    xTaskCreate(reboot_task,"reboot_task",4096,NULL,tskIDLE_PRIORITY,&reboot_task_hdl);
    xTaskCreate(daq_task, "daq_task", 4096, NULL, tskIDLE_PRIORITY, &daq_task_hdl);
    xTaskCreate(send_data_to_master_task,"send_data_to_master_task",4096,NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(process_master_cmds_task, "proccess_master_cmds_task", 4096, NULL, tskIDLE_PRIORITY, NULL); // this task needs to be last
    xTaskCreate(send_mail_to_node_task, "send_mail", 4096, NULL, tskIDLE_PRIORITY, NULL); // this task needs to be last

}

#endif