/*
 * SPDX-FileCopyrightText: 2010-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * The following example demonstrates a slave node in a TWAI network. The slave
 * node is responsible for sending data messages to the master. The example will
 * execute multiple iterations, with each iteration the slave node will do the
 * following:
 * 1) Start the TWAI driver
 * 2) Listen for ping messages from master, and send ping response
 * 3) Listen for start command from master
 * 4) Send data messages to master and listen for stop command
 * 5) Send stop response to master
 * 6) Stop the TWAI driver
 */

/*
 * NEW TWAI SLAVE - ESP-IDF 5.5.1 NODE DRIVER VERSION
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"

/* --------------------- Configuration ------------------ */

#define DATA_PERIOD_MS      50
#define NO_OF_ITERS         3
#define ITER_DELAY_MS      1000

#define RX_TASK_PRIO        8
#define TX_TASK_PRIO        9
#define CTRL_TSK_PRIO       10

#define TX_GPIO_NUM         25
#define RX_GPIO_NUM         26
#define TRANSM_RATE         1000000   // 1 Mbps

#define RX_QUEUE_LENGTH     16
#define TX_QUEUE_DEPTH      5

#define EXAMPLE_TAG         "TWAI Slave"

/* --------------------- CAN IDs ------------------------ */

#define ID_MASTER_STOP_CMD   0x0A0
#define ID_MASTER_START_CMD  0x0A1
#define ID_MASTER_PING       0x0A2
#define ID_SLAVE_STOP_RESP   0x0B0
#define ID_SLAVE_DATA        0x0B1
#define ID_SLAVE_PING_RESP  0x0B2

/* --------------------- Types -------------------------- */

typedef enum {
    TX_SEND_PING_RESP,
    TX_SEND_DATA,
    TX_SEND_STOP_RESP,
    TX_TASK_EXIT,
} tx_task_action_t;

typedef enum {
    RX_RECEIVE_PING,
    RX_RECEIVE_START_CMD,
    RX_RECEIVE_STOP_CMD,
    RX_TASK_EXIT,
} rx_task_action_t;

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

/* --------------------- RTOS Objects ------------------- */

static QueueHandle_t tx_task_queue;
static QueueHandle_t rx_task_queue;
static QueueHandle_t twai_rx_queue;

static SemaphoreHandle_t ctrl_task_sem;
static SemaphoreHandle_t stop_data_sem;
static SemaphoreHandle_t done_sem;

/* --------------------- TWAI Node ---------------------- */

static uint8_t ping_data[1];
static uint8_t stop_data[1];

static const twai_frame_t ping_resp = {
    .header.id = ID_SLAVE_PING_RESP,
    .header.ide = false,
    .buffer = ping_data,
    .buffer_len = 0,
};

static const twai_frame_t stop_resp = {
    .header.id = ID_SLAVE_STOP_RESP,
    .header.ide = false,
    .buffer = stop_data,
    .buffer_len = 0,
};

twai_node_handle_t node_hdl = NULL;

twai_onchip_node_config_t node_config = {
    .io_cfg.tx = TX_GPIO_NUM,
    .io_cfg.rx = RX_GPIO_NUM,
    .bit_timing.bitrate = TRANSM_RATE,
    .tx_queue_depth = TX_QUEUE_DEPTH,
};

/* --------------------- RX ISR Callback ---------------- */

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
        msg.dlc = frame.buffer_len;
        msg.data = 0;

        for (int i = 0; i < msg.dlc; i++) {
            msg.data |= ((uint64_t)raw[i] << (8 * i));
        }

        xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

/* --------------------- Blocking Receive --------------- */

esp_err_t twai_receive(can_rx_word_t *msg, TickType_t timeout)
{
    if (xQueueReceive(twai_rx_queue, msg, timeout) == pdTRUE)
        return ESP_OK;
    else
        return ESP_ERR_TIMEOUT;
}

/* --------------------- RX Task ------------------------ */

static void twai_receive_task(void *arg)
{
    while (1) {
        rx_task_action_t action;
        xQueueReceive(rx_task_queue, &action, portMAX_DELAY);

        if (action == RX_RECEIVE_PING) {
            while (1) {
                can_rx_word_t rx;
                twai_receive(&rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_PING) {
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        } 
        else if (action == RX_RECEIVE_START_CMD) {
            while (1) {
                can_rx_word_t rx;
                twai_receive(&rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_START_CMD) {
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        } 
        else if (action == RX_RECEIVE_STOP_CMD) {
            while (1) {
                can_rx_word_t rx;
                twai_receive(&rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_STOP_CMD) {
                    xSemaphoreGive(stop_data_sem);
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        } 
        else if (action == RX_TASK_EXIT) {
            break;
        }
    }
    vTaskDelete(NULL);
}

/* --------------------- TX Task ------------------------ */

static void twai_transmit_task(void *arg)
{
    while (1) {
        tx_task_action_t action;
        xQueueReceive(tx_task_queue, &action, portMAX_DELAY);

        if (action == TX_SEND_PING_RESP) {
            twai_node_transmit(node_hdl, &ping_resp, portMAX_DELAY);
            ESP_LOGI(EXAMPLE_TAG, "Ping response sent");
            xSemaphoreGive(ctrl_task_sem);
        } 
        else if (action == TX_SEND_DATA) {
            ESP_LOGI(EXAMPLE_TAG, "Start sending data");

            while (1) {
                int32_t timestamp = (int32_t)xTaskGetTickCount();
                int32_t value = timestamp + 100;   // example payload

                uint64_t packed =
                      ((uint64_t)(uint32_t)timestamp)
                    | ((uint64_t)(uint32_t)value << 32);

                uint8_t raw[8];
                memcpy(raw, &packed, 8);

                twai_frame_t tx = {
                    .header.id = ID_SLAVE_DATA,
                    .header.ide = false,
                    .buffer = raw,
                    .buffer_len = 8,
                };

                twai_node_transmit(node_hdl, &tx, portMAX_DELAY);
                ESP_LOGI(EXAMPLE_TAG,
                         "TX Data | ts=%" PRIi32 " val=%" PRIi32,
                         timestamp, value);

                vTaskDelay(pdMS_TO_TICKS(DATA_PERIOD_MS));

                if (xSemaphoreTake(stop_data_sem, 0) == pdTRUE)
                    break;
            }
        } 
        else if (action == TX_SEND_STOP_RESP) {
            twai_node_transmit(node_hdl, &stop_resp, portMAX_DELAY);
            ESP_LOGI(EXAMPLE_TAG, "Stop response sent");
            xSemaphoreGive(ctrl_task_sem);
        } 
        else if (action == TX_TASK_EXIT) {
            break;
        }
    }
    vTaskDelete(NULL);
}

/* --------------------- Control Task ------------------- */

static void twai_control_task(void *arg)
{
    xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

    for (int iter = 0; iter < NO_OF_ITERS; iter++) {

        ESP_ERROR_CHECK(twai_node_enable(node_hdl));
        ESP_LOGI(EXAMPLE_TAG, "Driver started");

        rx_task_action_t rx;
        tx_task_action_t tx;

        rx = RX_RECEIVE_PING;
        xQueueSend(rx_task_queue, &rx, portMAX_DELAY);
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

        tx = TX_SEND_PING_RESP;
        xQueueSend(tx_task_queue, &tx, portMAX_DELAY);
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

        rx = RX_RECEIVE_START_CMD;
        xQueueSend(rx_task_queue, &rx, portMAX_DELAY);
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

        tx = TX_SEND_DATA;
        rx = RX_RECEIVE_STOP_CMD;
        xQueueSend(tx_task_queue, &tx, portMAX_DELAY);
        xQueueSend(rx_task_queue, &rx, portMAX_DELAY);
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

        tx = TX_SEND_STOP_RESP;
        xQueueSend(tx_task_queue, &tx, portMAX_DELAY);
        xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

        ESP_ERROR_CHECK(twai_node_disable(node_hdl));
        ESP_LOGI(EXAMPLE_TAG, "Driver stopped");

        vTaskDelay(pdMS_TO_TICKS(ITER_DELAY_MS));
    }

    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

/* --------------------- app_main ----------------------- */

void app_main(void)
{
    //Add short delay to allow master it to initialize first 
    for (int i = 3; i > 0; i--) { 
        printf("Slave starting in %d\n", i); 
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }

    tx_task_queue = xQueueCreate(1, sizeof(tx_task_action_t));
    rx_task_queue = xQueueCreate(1, sizeof(rx_task_action_t));
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));

    ctrl_task_sem = xSemaphoreCreateBinary();
    stop_data_sem = xSemaphoreCreateBinary();
    done_sem = xSemaphoreCreateBinary();

    xTaskCreate(twai_receive_task, "TWAI_rx", 4096, NULL, RX_TASK_PRIO, NULL);
    xTaskCreate(twai_transmit_task, "TWAI_tx", 4096, NULL, TX_TASK_PRIO, NULL);
    xTaskCreate(twai_control_task,  "TWAI_ctrl", 4096, NULL, CTRL_TSK_PRIO, NULL);

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

    xSemaphoreGive(ctrl_task_sem);
    xSemaphoreTake(done_sem, portMAX_DELAY);

    ESP_ERROR_CHECK(twai_node_delete(node_hdl));
}

