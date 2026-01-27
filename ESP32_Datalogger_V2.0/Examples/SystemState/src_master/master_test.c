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

void read_button(void *args, bool on_rising_edge){
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



void send_control_cmd(control_cmd_t state){
    control_msg_data[0] = state;
    ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &control_message, 0));  // Timeout = 0: returns immediately if queue is full
}

void init_node(void){
    // Create a new TWAI controller driver instance
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    // Start the TWAI controller
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));

    ESP_ERROR_CHECK(twai_node_disable(node_hdl));
    // ESP_ERROR_CHECK must alwasy be insde a scope { }
}

void init_control_panel(void){
    // Buttons
    set_button_config(BTN_GPIO_NUM);

    // LEDS
    set_LED_config(LED_GPIO_NUM);
}

// Task Notification System
static TaskHandle_t xTaskToNotify = NULL;
const UBaseType_t xArrayIndex = 0; // Task notification array
/* The peripheral driver's transmit function. */
void StartTransmission( uint8_t *pcData, size_t xDataLength )
{
    /* At this point xTaskToNotify should be NULL as no transmission
       is in progress. A mutex can be used to guard access to the
       peripheral if necessary. */
    configASSERT( xTaskToNotify == NULL );

    /* Store the handle of the calling task. */
    xTaskToNotify = xTaskGetCurrentTaskHandle();

    /* Start the transmission - an interrupt is generated when the
       transmission is complete. */
    vStartTransmit( pcData, xDataLength );
}
/* The task that initiates the transmission, then enters the
   Blocked state (so not consuming any CPU time) to wait for it
   to complete. */
void vAFunctionCalledFromATask( uint8_t ucDataToTransmit,
                                size_t xDataLength ){
uint32_t ulNotificationValue;
const TickType_t xMaxBlockTime = pdMS_TO_TICKS( 200 );

    /* Start the transmission by calling the function shown above. */
    StartTransmission( ucDataToTransmit, xDataLength );

    /* Wait to be notified that the transmission is complete. Note
       the first parameter is pdTRUE, which has the effect of clearing
       the task's notification value back to 0, making the notification
       value act like a binary (rather than a counting) semaphore. */
    ulNotificationValue = ulTaskNotifyTakeIndexed( xArrayIndex,pdTRUE,xMaxBlockTime );

    if( ulNotificationValue == 1 )
    {
        /* The transmission ended as expected. */
    }
    else
    {
        /* The call to ulTaskNotifyTake() timed out. */
    }
}
void LedNotifyTask(void *arg)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ToggleLed();
  }
}
void app_main(void)
{
    int test = 2;
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
        is_sampling = false;
        ESP_LOGI("TEST 3","Sampling Control on esp32p4 with TaskNotification");
        // Make sure configUSE_TASK_NOTIFICATIONS = 1;
        xTaskCreate(record_data,"Record Data 2", 4096, NULL,tskIDLE_PRIORITY,xTaskToNotify);

        break;
    case 5:
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