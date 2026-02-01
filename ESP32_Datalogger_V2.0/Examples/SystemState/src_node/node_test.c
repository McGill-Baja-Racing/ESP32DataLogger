#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include <inttypes.h>
#include "esp_timer.h"
#include <string.h>

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"

#include <freertos/projdefs.h>

#include "../lib/can/frames.c"



/* --------------------- Definitions and static variables ------------------ */


#define TX_GPIO_NUM             20              // CAN TX Pin
#define RX_GPIO_NUM             21              // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define TAG             "Node"
#define NODE_ID                 0               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       100 

#define ID_DATA                 (0x0B1 + NODE_ID)
#define LED_GPIO_NUM            0

control_cmd_t CTRL_STATUS;

static bool twai_rx_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    uint8_t recv_buff[8];
    twai_frame_t rx_frame = {
        .buffer = recv_buff,
        .buffer_len = sizeof(recv_buff),
    };
    if (ESP_OK == twai_node_receive_from_isr(handle, &rx_frame)) {
        ESP_LOGI(TAG,"Received Message");
        // receive ok, do something here
        CTRL_STATUS = (control_cmd_t)recv_buff;
    }else{
        CTRL_STATUS = STOP_CMD;
    }
    
    return false;
}

twai_event_callbacks_t user_cbs = {
    .on_rx_done = twai_rx_cb,
};
void set_button_config(gpio_num_t gpio_num){
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);
}

void set_LED_config(gpio_num_t gpio_num){
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num,0);
}
// Node Setup
twai_node_handle_t node_hdl = NULL; // pointer to TWAI node (TWAI instance)
twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,             // TWAI TX GPIO pin
    .io_cfg.rx = RX_GPIO_NUM,             // TWAI RX GPIO pin
    .bit_timing.bitrate = TRANSM_RATE,    // bps bitrate
    .tx_queue_depth = TX_QUEUE_DEPTH,     // Transmit queue depth set to 5
    // .clk_src, is set to the default one on the chip.
};


void init_node(void){
    // Create a new TWAI controller driver instance
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(TAG, "Driver installed");

    // Start the TWAI controller

    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &user_cbs, NULL));

    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "Node Enabled");

    
    // ESP_ERROR_CHECK must alwasy be insde a scope { }
}



// Receive Event Callback



void record_data(void *args){
    int led_state = 0;
    set_LED_config(LED_GPIO_NUM);
    while(CTRL_STATUS==START_CMD){
        gpio_set_level(LED_GPIO_NUM,led_state);
        vTaskDelay(pdMS_TO_TICKS(SAMPLING_SPEED_MS));
        led_state = !led_state;
    }
}
void app_main(void)
{   
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG,"Running");
    int test = 4;
    init_node();
    // Code that ask for user input
    switch (test)
    {
    case 4: // Test Communication
        break;
    case 5: // See if simulation sampling works
        xTaskCreate(record_data,"Sampling Data", 4096, NULL,tskIDLE_PRIORITY,NULL);
        break;
    default:
        break;
    }

}


/*
twai_mask_filter_config_t mfilter_cfg = {
    .id = 0x10,         // 0b 000 0001 0000
    .mask = 0x7f0,      // 0b 111 1111 0000 — the upper 7 bits must match strictly, the lower 4 bits are ignored, accepts IDs of the form
                        // 0b 000 0001 xxxx (hex 0x01x)
    .is_ext = false,    // Accept only standard IDs, not extended IDs
};
ESP_ERROR_CHECK(twai_node_config_mask_filter(node_hdl, 0, &mfilter_cfg))*/