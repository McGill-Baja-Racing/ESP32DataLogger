/*
 * SPDX-FileCopyrightText: 2010-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * The following example demonstrates a master node in a TWAI network. The master
 * node is responsible for initiating and stopping the transfer of data messages.
 * The example will execute multiple iterations, with each iteration the master
 * node will do the following:
 * 1) Start the TWAI driver
 * 2) Repeatedly send ping messages until a ping response from slave is received
 * 3) Send start command to slave and receive data messages from slave
 * 4) Send stop command to slave and wait for stop response from slave
 * 5) Stop the TWAI driver
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include <inttypes.h>

#include "esp_twai.h"
#include "esp_twai_onchip.h"

/* --------------------- Definitions and static variables ------------------ */
//Example Configuration
#define PING_PERIOD_MS          250
#define NO_OF_DATA_MSGS         50
#define NO_OF_ITERS             3
#define ITER_DELAY_MS           1000
#define RX_TASK_PRIO            8
#define TX_TASK_PRIO            9
#define CTRL_TSK_PRIO           10


#define TX_GPIO_NUM             5               // CAN TX Pin
#define RX_GPIO_NUM             4               // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define EXAMPLE_TAG             "TWAI Master"

#define ID_MASTER_STOP_CMD      0x0A0
#define ID_MASTER_START_CMD     0x0A1
#define ID_MASTER_PING          0x0A2
#define ID_SLAVE_STOP_RESP      0x0B0
#define ID_SLAVE_DATA           0x0B1
#define ID_SLAVE_PING_RESP      0x0B2

typedef enum {
    TX_SEND_PINGS,
    TX_SEND_START_CMD,
    TX_SEND_STOP_CMD,
    TX_TASK_EXIT,
} tx_task_action_t;

typedef enum {
    RX_RECEIVE_PING_RESP,
    RX_RECEIVE_DATA,
    RX_RECEIVE_STOP_RESP,
    RX_TASK_EXIT,
} rx_task_action_t;

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

static QueueHandle_t tx_task_queue;
static QueueHandle_t rx_task_queue;
static SemaphoreHandle_t stop_ping_sem;
static SemaphoreHandle_t ctrl_task_sem;
static SemaphoreHandle_t done_sem;
static QueueHandle_t twai_rx_queue;

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

esp_err_t twai_receive(can_rx_word_t *msg, TickType_t timeout)
{
    if (xQueueReceive(twai_rx_queue, msg, timeout) == pdTRUE)
        return ESP_OK;
    else
        return ESP_ERR_TIMEOUT;
}

/* --------------------------- New Library Functions -------------------------- */



static uint8_t ping_data[1];
static uint8_t start_data[1];
static uint8_t stop_data[1];


twai_node_handle_t node_hdl = NULL;
twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,             // TWAI TX GPIO pin
    .io_cfg.rx = RX_GPIO_NUM,             // TWAI RX GPIO pin
    .bit_timing.bitrate = TRANSM_RATE,    // bps bitrate
    .tx_queue_depth = TX_QUEUE_DEPTH,     // Transmit queue depth set to 5
};

static const twai_frame_t ping_message = {
    .header.id = ID_MASTER_PING,    // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = ping_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

static const twai_frame_t start_message = {
    .header.id = ID_MASTER_START_CMD,    // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = start_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

static const twai_frame_t stop_message = {
    .header.id = ID_MASTER_STOP_CMD,     // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = stop_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

// Receive from CAN ISR handler
twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};




/* --------------------------- Tasks and Functions -------------------------- */

static void twai_receive_task(void *arg)
{
    while (1) {
        rx_task_action_t action;
        xQueueReceive(rx_task_queue, &action, portMAX_DELAY);

        if (action == RX_RECEIVE_PING_RESP) {
            //Listen for ping response from slave
            while (1) {
                can_rx_word_t rx_msg;
                twai_receive(&rx_msg, portMAX_DELAY);
                if (rx_msg.id == ID_SLAVE_PING_RESP) {
                    xSemaphoreGive(stop_ping_sem);
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        } else if (action == RX_RECEIVE_DATA) {
            //Receive data messages from slave
            uint32_t data_msgs_rec = 0;
            while (data_msgs_rec < NO_OF_DATA_MSGS) {
                can_rx_word_t rx_msg;
                twai_receive(&rx_msg, portMAX_DELAY);
                if (rx_msg.id == ID_SLAVE_DATA) {
                    
                    int32_t value       = (int32_t)( rx_msg.data    & 0xFFFFFFFF );
                    int32_t timestamp   = (int32_t)((rx_msg.data >> 32) & 0xFFFFFFFF );

                    ESP_LOGI(EXAMPLE_TAG,
                        "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32 " | Value: %" PRIi32,
                        rx_msg.id,
                        timestamp,
                        value);

                    data_msgs_rec ++;
                }
            }
            xSemaphoreGive(ctrl_task_sem);
        } else if (action == RX_RECEIVE_STOP_RESP) {
            //Listen for stop response from slave
            while (1) {
                can_rx_word_t rx_msg;
                twai_receive(&rx_msg, portMAX_DELAY);
                if (rx_msg.id == ID_SLAVE_STOP_RESP) {
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        } else if (action == RX_TASK_EXIT) {
            break;
        }
    }
    vTaskDelete(NULL);
}

static void twai_transmit_task(void *arg)
{
    while (1) {
        tx_task_action_t action;
        xQueueReceive(tx_task_queue, &action, portMAX_DELAY);

        if (action == TX_SEND_PINGS) {
            //Repeatedly transmit pings
            ESP_LOGI(EXAMPLE_TAG, "Transmitting ping");
            while (xSemaphoreTake(stop_ping_sem, 0) != pdTRUE) {
                twai_node_transmit(node_hdl, &ping_message, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(PING_PERIOD_MS));
            }
        } else if (action == TX_SEND_START_CMD) {
            //Transmit start command to slave
            twai_node_transmit(node_hdl, &start_message, portMAX_DELAY);
            ESP_LOGI(EXAMPLE_TAG, "Transmitted start command");
        } else if (action == TX_SEND_STOP_CMD) {
            //Transmit stop command to slave
            twai_node_transmit(node_hdl, &stop_message, portMAX_DELAY);
            ESP_LOGI(EXAMPLE_TAG, "Transmitted stop command");
        } else if (action == TX_TASK_EXIT) {
            break;
        }
    }
    vTaskDelete(NULL);
}

static void twai_control_task(void *arg)
{
    xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);
    tx_task_action_t tx_action;
    rx_task_action_t rx_action;

    for (int iter = 0; iter < NO_OF_ITERS; iter++) {
        // Start the TWAI controller
        ESP_ERROR_CHECK(twai_node_enable(node_hdl));
        ESP_LOGI(EXAMPLE_TAG, "Driver started");

        //Start transmitting pings, and listen for ping response
        tx_action = TX_SEND_PINGS;
        rx_action = RX_RECEIVE_PING_RESP;
        xQueueSend(tx_task_queue, &tx_action, portMAX_DELAY);
        xQueueSend(rx_task_queue, &rx_action, portMAX_DELAY);

        //Send Start command to slave, and receive data messages
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);
        tx_action = TX_SEND_START_CMD;
        rx_action = RX_RECEIVE_DATA;
        xQueueSend(tx_task_queue, &tx_action, portMAX_DELAY);
        xQueueSend(rx_task_queue, &rx_action, portMAX_DELAY);

        //Send Stop command to slave when enough data messages have been received. Wait for stop response
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);
        tx_action = TX_SEND_STOP_CMD;
        rx_action = RX_RECEIVE_STOP_RESP;
        xQueueSend(tx_task_queue, &tx_action, portMAX_DELAY);
        xQueueSend(rx_task_queue, &rx_action, portMAX_DELAY);

        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);
        ESP_ERROR_CHECK(twai_node_disable(node_hdl));
        ESP_LOGI(EXAMPLE_TAG, "Driver stopped");
        vTaskDelay(pdMS_TO_TICKS(ITER_DELAY_MS));
    }
    //Stop TX and RX tasks
    tx_action = TX_TASK_EXIT;
    rx_action = RX_TASK_EXIT;
    xQueueSend(tx_task_queue, &tx_action, portMAX_DELAY);
    xQueueSend(rx_task_queue, &rx_action, portMAX_DELAY);

    //Delete Control task
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

void app_main(void)
{
    //Create tasks, queues, and semaphores
    rx_task_queue = xQueueCreate(1, sizeof(rx_task_action_t));
    tx_task_queue = xQueueCreate(1, sizeof(tx_task_action_t));
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    ctrl_task_sem = xSemaphoreCreateBinary();
    stop_ping_sem = xSemaphoreCreateBinary();
    done_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(twai_receive_task, "TWAI_rx", 4096, NULL, RX_TASK_PRIO, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(twai_transmit_task, "TWAI_tx", 4096, NULL, TX_TASK_PRIO, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(twai_control_task, "TWAI_ctrl", 4096, NULL, CTRL_TSK_PRIO, NULL, tskNO_AFFINITY);

    //Install TWAI driver
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(EXAMPLE_TAG, "Driver installed");
    
    // Handle CAN receive ISR
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

    xSemaphoreGive(ctrl_task_sem);              //Start control task
    xSemaphoreTake(done_sem, portMAX_DELAY);    //Wait for completion

    //Uninstall TWAI driver
    ESP_ERROR_CHECK(twai_node_delete(node_hdl));
    ESP_LOGI(EXAMPLE_TAG, "Driver uninstalled");

    //Cleanup
    vQueueDelete(rx_task_queue);
    vQueueDelete(tx_task_queue);
    vSemaphoreDelete(ctrl_task_sem);
    vSemaphoreDelete(stop_ping_sem);
    vSemaphoreDelete(done_sem);
}





