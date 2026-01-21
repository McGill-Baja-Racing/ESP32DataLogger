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


// Control Panel and LED Display Config
#define PLAY_BTN_GPIO_NUM       1
#define RESET_BTN_GPIO_NUM      2
#define ERR_GPIO_NUM            3
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
static uint8_t prev_play_btn_state = 0;
static uint8_t prev_reset_btn_state = 0;
static uint8_t is_samping = 0;
static uint8_t is_reseting = 0;
static uint8_t led_state = 0;


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

void control_pannel(void *args){
    int prev_reset_state = gpio_get_level(RESET_BTN_GPIO_NUM);
    int curr_reset_state = prev_reset_state;
    int prev_play_btn_state = 0;
    while (true){
        is_samping = gpio_get_level(PLAY_BTN_GPIO_NUM);

        if (prev_play_btn_state!=is_samping){ // Sampling Status LOG
            ESP_LOGI(TAG, "Sampling Status %s!", is_samping == true ? "ON" : "OFF");
            prev_play_btn_state = is_samping;
        }

        if (is_samping) {
            
            curr_reset_state = gpio_get_level(RESET_BTN_GPIO_NUM);

            if (prev_reset_state != curr_reset_state){
                ESP_LOGI(TAG, "Resetting Error - LED");
                vTaskDelay(BLINK_DUR/portTICK_PERIOD_MS);
                prev_reset_btn_state = curr_reset_state;
                is_reseting = false;
            }
        }
    }
}

void display_pannel(void *args){
    if (is_reseting){
        ESP_LOGI(TAG, "Turning LED!", led_state == true ? "ON" : "OFF");
        gpio_set_level(ERR_GPIO_NUM,led_state);
        led_state = !led_state;
        vTaskDelay(BLINK_FREQ/portTICK_PERIOD_MS);
    }
    gpio_set_level(ERR_GPIO_NUM,0);// Turning off Warning Sign
}

void app_main(void)
{
    init_node();
    init_control_panel();

    xTaskCreate(control_pannel,"Control Panel", 4096, NULL,tskIDLE_PRIORITY,NULL);
    xTaskCreate(display_pannel,"Display Banel", 4096, NULL,tskIDLE_PRIORITY,NULL);
}



