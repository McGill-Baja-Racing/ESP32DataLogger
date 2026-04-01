#ifdef MASTER

#include "master_btn_control.hpp"

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

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "logger";

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
#define TX_QUEUE_DEPTH          5
#define RX_QUEUE_LENGTH         256

/* --------------------- BTN config ------------------ */


#define CTRL_BTN_GPIO_NUM       3
#define ISR_BTN_QUEUE_LENGTH    2

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

static QueueHandle_t twai_rx_queue = NULL;
static twai_node_handle_t node_hdl = NULL;

static twai_onchip_node_config_t node_config = {
    .io_cfg = {
        .tx = (gpio_num_t)TX_GPIO_NUM,
        .rx = (gpio_num_t)RX_GPIO_NUM,
    },
    .bit_timing = {
        .bitrate = TRANSM_RATE,
    },
    .tx_queue_depth = TX_QUEUE_DEPTH,
};

/* ---------------------- Button Params -------------------- */
static TaskHandle_t message_slave_hdl;

static void IRAM_ATTR ctlr_btn_handler(void* arg)
{
    // NOTE:for aditional buttons just create more task and add as input the button pin! then a switch case for the differnet taskhandlers to notify!
    BaseType_t hp_woken = pdFALSE;
    vTaskNotifyGiveFromISR(message_slave_hdl, &hp_woken); // notify the task
    if (hp_woken) portYIELD_FROM_ISR();
}

// Acts as interrupt for the button Press
void ctrl_btn_init(void){
    // NOTE: Please do not use the interrupt of GPIO36 and GPIO39 when using ADC or Wi-Fi and Bluetooth with sleep mode enabled. 
    // Look into circuit component to better understand rising edge
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << CTRL_BTN_GPIO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE // defined interrupt
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_LOGI("Btn","Config Activated");
}

static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_task_woken = pdFALSE;

    uint8_t raw[8] = {0};
    twai_frame_t frame = {};
    frame.buffer     = raw;
    frame.buffer_len = sizeof(raw);

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {
        can_rx_word_t msg = {};
        msg.id = frame.header.id;

        uint8_t dlc = frame.header.dlc;
        if (dlc > 8) dlc = 8;
        msg.dlc = dlc;

        uint64_t packed = 0;
        for (int i = 0; i < msg.dlc; i++) {
            packed |= (uint64_t)raw[i] << (8 * i); // little-endian packing
        }
        msg.data = packed;

        (void)xQueueSendFromISR(twai_rx_queue, &msg, &hp_task_woken);
        if (hp_task_woken) {
            portYIELD_FROM_ISR();
        }
    }

    return hp_task_woken == pdTRUE; // if hp_task_woken: higher priority task has woken up
}

static twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

/* --------------------- Button Control ------------------ */

/* ------------------- Start and Stop ---------------------- */
void v_send_control_cmd(void *args ){
    bool status = false;
    while(1){
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)); {
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

/* --------------------- app_main ------------------ */
void btn_control_main(void)
{   
    ESP_LOGI(TAG, "Control Button Initialisation");
    ctrl_btn_init();
    gpio_install_isr_service(0);

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled");

    ESP_LOGI(TAG, "Data logger started.");
    xTaskCreate(v_send_control_cmd, "btn_task", 4096, NULL, 10, &message_slave_hdl);

    gpio_isr_handler_add((gpio_num_t)CTRL_BTN_GPIO_NUM, ctlr_btn_handler, NULL);
    // plug the gpio pin to the button dirrectly and on the other side of the button connect it to ground
}


#endif
