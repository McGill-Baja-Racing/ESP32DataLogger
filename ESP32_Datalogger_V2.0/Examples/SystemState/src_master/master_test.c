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


// Master config
#define TX_GPIO_NUM             5               // CAN TX Pin
#define RX_GPIO_NUM             4               // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define TAG                     "Master"
#define NODE_ID                 0               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       500
#define TIME_BEACON_PERIOD_MS   100           


#define BTN_GPIO_NUM            31
#define LED_GPIO_NUM            52

bool is_sampling = false;
bool on_rising_edge = false;

// Node Setup
twai_node_handle_t node_hdl = NULL; // pointer to TWAI node (TWAI instance)
twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,             // TWAI TX GPIO pin
    .io_cfg.rx = RX_GPIO_NUM,             // TWAI RX GPIO pin
    .bit_timing.bitrate = TRANSM_RATE,    // bps bitrate
    .tx_queue_depth = TX_QUEUE_DEPTH,     // Transmit queue depth set to 5
    // .clk_src, is set to the default one on the chip.
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

void record_data(void *args){
    int led_state = 0;
    set_LED_config(LED_GPIO_NUM);
    while(is_sampling){
        gpio_set_level(LED_GPIO_NUM,led_state);
        vTaskDelay(pdMS_TO_TICKS(SAMPLING_SPEED_MS));
        led_state = !led_state;
    }
}

void read_button(void *args){\
    set_button_config(BTN_GPIO_NUM);
    int prev_state = 0;
    int curr_state = 0;
    bool is_btn_pressed = false;
    for(;;){
        curr_state = gpio_get_level(BTN_GPIO_NUM);

        if (on_rising_edge){
            is_btn_pressed = (prev_state ==1 && curr_state ==0);

        }else{
            is_btn_pressed = (prev_state ==1 && curr_state ==0);

        }

        if (is_btn_pressed){
            ESP_LOGI(TAG,"%s Sampling",is_sampling? "Start":"Stop");
            is_sampling =!is_sampling;
            prev_state = curr_state;
            is_btn_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}



void send_control_cmd(void *args ){
    while(1){

    control_msg_data[0] = (control_cmd_t)args;
    ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &control_message, 0));  // Timeout = 0: returns immediately if queue is full
    ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));  // Wait for transmission to finish
    }

}

void init_node(void){
    // Create a new TWAI controller driver instance
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    // Start the TWAI controller
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    //ESP_ERROR_CHECK(twai_node_disable(node_hdl));
    // ESP_ERROR_CHECK must alwasy be insde a scope { }
}

void init_control_panel(void){
    // Buttons
    set_button_config(BTN_GPIO_NUM);

    // LEDS
    set_LED_config(LED_GPIO_NUM);
}

void app_main(void)
{
    int test = 4;
    init_node();
    init_control_panel();
    // Code that ask for user input
    switch (test)
    {
    case 1: // DONE
        is_sampling = true; // LED is initially On
        ESP_LOGI("TEST 1","Sampling Data - Blinking LED"); 
        xTaskCreate(record_data,"Record Data 1", 4096, NULL,tskIDLE_PRIORITY,NULL);
        break;
    case 2: // DONE
        ESP_LOGI("TEST 2","Read Button"); 
        /* BUTTON Wiring Config (out needs to be connected to resisotr --. when you press button all 4 terminals are in contact
           Null       Out
                -----
                | O |
                -----
            GND       3V3
            
            Also what cause the device to power off is when you short it.
                - Digital pin only cares about the voltage so ideally you want to reduce the current going in. (It already has a strong resistor when read is activated)
            Make sure to never leave a terminal floating -> will not be able to read
        */ 
        xTaskCreate(read_button,"Button Panel", 4096, NULL,tskIDLE_PRIORITY,NULL);
        break;
    case 3:
        is_sampling = false; // LED is initially Off
        ESP_LOGI("TEST 3","Sampling Control on esp32p4 with button polling");
        xTaskCreate(record_data,"Record Data 2", 4096, NULL,tskIDLE_PRIORITY,NULL);
        xTaskCreate(read_button,"Button Panel", 4096, NULL,tskIDLE_PRIORITY,NULL);
        break;
    case 4:
        ESP_LOGI("TEST 4","Send Message vi CAN bus");
        xTaskCreate(send_control_cmd,"Start Sampling",4096,(void *) STOP_CMD,tskIDLE_PRIORITY,NULL);
        break;
    case 5:
        is_sampling = false;
        ESP_LOGI("TEST 3","Sampling Control on esp32p4 with TaskNotification");
        // Make sure configUSE_TASK_NOTIFICATIONS = 1;
        break;
    case 6:
        ESP_LOGI("TEST 4","CAN Sampling Control with esp32c3");
        break;

    default:
        break;
    }

}

// LOOK INTO Event-driven programming --> look into group event since all the task will be woken up
/*

Search: 
interrupt driven buttons FreeRTOS
GPIO interrupt FreeRTOS

FreeRTOS blocking task button
avoid busy wait FreeRTOS

FreeRTOS task notification from ISR button

Debouncing (hardware or software)

ISR-to-task signaling

Low-power / tickless idle

Cooperative vs preemptive scheduling
*/

// TEST 2.B: Use task notification -> https://ece353.engr.wisc.edu/freertos/task-notifications/#:~:text=Choosing%20between%20a%20Task%20Notification%2C%20a%20semaphore%2C,Notification%20is%20often%20the%20most%20efficient%20choice.