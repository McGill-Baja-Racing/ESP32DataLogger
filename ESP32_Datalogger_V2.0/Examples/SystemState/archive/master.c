#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/projdefs.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"

#include "../lib/can/frames.c"

// Control Panel and LED Display Config
#define PLAY_BTN_GPIO_NUM       31
#define RESET_BTN_GPIO_NUM      51
#define ERR_GPIO_NUM            52
#define FAUL_DELAY              2000
#define BLINK_DUR               10000
#define BLINK_FREQ              500

// Master config
#define TX_GPIO_NUM             5               // CAN TX Pin
#define RX_GPIO_NUM             4               // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define TAG                     "Master"
#define NODE_ID                 0               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       100 
#define TIME_BEACON_PERIOD_MS   100           


// Frame config
#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_MASTER_TIME_BEACON   0x0A2

// Node Setup
twai_node_handle_t node_hdl = NULL; // pointer to TWAI node (TWAI instance)
twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,             // TWAI TX GPIO pin
    .io_cfg.rx = RX_GPIO_NUM,             // TWAI RX GPIO pin
    .bit_timing.bitrate = TRANSM_RATE,    // bps bitrate
    .tx_queue_depth = TX_QUEUE_DEPTH,     // Transmit queue depth set to 5
    // .clk_src, is set to the default one on the chip.
};

// Control Panel Variables
static uint8_t is_sampling = 0;
static uint8_t is_reseting = 0;
static uint8_t state = 0;
static uint8_t simulate = true;
// state = 0 for all tasks is stop --> pause or itermediate step

void set_button_config(gpio_num_t gpio_num){
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);
}

void set_LED_config(gpio_num_t gpio_num){
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num,0);
}

void init_control_panel(void){
    // Buttons
    set_button_config(PLAY_BTN_GPIO_NUM);
    set_button_config(RESET_BTN_GPIO_NUM);

    // LEDS
    set_LED_config(ERR_GPIO_NUM);
}

void init_node(void){
    // Create a new TWAI controller driver instance
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    // Start the TWAI controller
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));

    ESP_ERROR_CHECK(twai_node_disable(node_hdl));
    // ESP_ERROR_CHECK must alwasy be insde a scope { }
}

int is_push_btn_pressed(gpio_num_t btn_pin ,int button_prev_state){
    // TODO make input a pointer
    int curr_state =  gpio_get_level(btn_pin);
    // update input value
    return curr_state != button_prev_state;
}

void control_panel(void *args){
    int prev_play_btn_state = 0;
    int prev_reset_btn_state = 0;
    int local_state = 0;
    while (1){

        is_sampling = is_push_btn_pressed(PLAY_BTN_GPIO_NUM,prev_play_btn_state);

        if (is_sampling){
            prev_play_btn_state = !prev_play_btn_state;
            local_state = 0;
            state = 0;
        }

        switch (local_state)
        {
        case 0:
            ESP_LOGI(TAG,"Begin Sampling Data");
            local_state = 1;
            break;
        case 1:
            vTaskDelay(pdMS_TO_TICKS(FAUL_DELAY));
            ESP_LOGI(TAG, "Fault Detected");
            state = 1;
            local_state = 2;
            break;
        case 2:
            is_reseting = is_push_btn_pressed(RESET_BTN_GPIO_NUM,prev_reset_btn_state);

            if(is_reseting){
                ESP_LOGI(TAG, "Resetting Errors Status");
                prev_reset_btn_state = !prev_reset_btn_state;
                state = 2;
                vTaskDelay(pdMS_TO_TICKS(BLINK_DUR));
                state = 3;
                local_state = -1;
            }
            break;
        default:
            break;
        }
    }
}

void fault_display(void* args){ // GOOD
    int led_state = 0; 
    while(1){
         
        switch (state)
        {
        case 0:
            /* code */
            break;
        case 1: // FAULT DETECTED
            ESP_LOGI(TAG,"Turn Error Led On");
            gpio_set_level(ERR_GPIO_NUM, 1);
            state = 0;
        case 2: // RESETING - BLINKING
            led_state = !led_state;
            gpio_set_level(ERR_GPIO_NUM, led_state);
            vTaskDelay(pdMS_TO_TICKS(BLINK_FREQ));
            break;
        case 3: // RESET - RESTARTING CHECK
            ESP_LOGI(TAG,"Systems Test Was Successfull.");
            gpio_set_level(ERR_GPIO_NUM, 0);
            state = 0;
            break;
        default:
            break;
        };
    }

}

void app_main(void)
{
    init_node();
    init_control_panel();
    xTaskCreate(control_panel,"Control Panel", 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(fault_display,"Display Panel", 4096, NULL,tskIDLE_PRIORITY,NULL);
}