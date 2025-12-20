/*
 * TWAI SLAVE – PARAMETERIZED
 * Original logic preserved
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

/* ---------------- CONFIG ---------------- */

#define SLAVE_ID                2   // CHANGE TO 2 FOR SECOND SLAVE

#define DATA_PERIOD_MS          50
#define NO_OF_ITERS             1
#define ITER_DELAY_MS           1000

#define RX_TASK_PRIO            8
#define TX_TASK_PRIO            9
#define CTRL_TSK_PRIO           10

#define TX_GPIO_NUM             21
#define RX_GPIO_NUM             20
#define TRANSM_RATE             1000000

#define RX_QUEUE_LENGTH         16
#define TX_QUEUE_DEPTH          5

#define EXAMPLE_TAG             "TWAI_Slave"

/* ---------------- CAN IDs ---------------- */

#define ID_MASTER_PING              0x0A2
#define ID_MASTER_START_CMD         0x0A1
#define ID_MASTER_STOP_CMD(slave)   (0x0A0 + (slave))

#define ID_SLAVE_PING_RESP          (0x0B2 + SLAVE_ID)
#define ID_SLAVE_DATA               (0x0B1 + SLAVE_ID)
#define ID_SLAVE_STOP_RESP          (0x0B0 + SLAVE_ID)

/* ---------------- TYPES ---------------- */

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
    uint64_t data;
} can_rx_word_t;

/* ---------------- GLOBALS ---------------- */

static QueueHandle_t tx_task_queue;
static QueueHandle_t rx_task_queue;
static QueueHandle_t twai_rx_queue;

static SemaphoreHandle_t ctrl_task_sem;
static SemaphoreHandle_t stop_data_sem;
static SemaphoreHandle_t done_sem;

twai_node_handle_t node_hdl;

/* ---------------- RX ISR ---------------- */

static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    BaseType_t hp = pdFALSE;

    uint8_t raw[8];
    twai_frame_t frame = {
        .buffer = raw,
        .buffer_len = sizeof(raw),
    };

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {
        can_rx_word_t msg = {
            .id = frame.header.id,
            .data = 0,
        };
        for (int i = 0; i < frame.buffer_len; i++)
            msg.data |= ((uint64_t)raw[i] << (8 * i));

        xQueueSendFromISR(twai_rx_queue, &msg, &hp);
    }
    return hp == pdTRUE;
}

/* ---------------- RX TASK ---------------- */

static void twai_receive_task(void *arg)
{
    while (1) {
        rx_task_action_t action;
        xQueueReceive(rx_task_queue, &action, portMAX_DELAY);

        if (action == RX_RECEIVE_PING) {
            while (1) {
                can_rx_word_t rx;
                xQueueReceive(twai_rx_queue, &rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_PING) {
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        }

        else if (action == RX_RECEIVE_START_CMD) {
            while (1) {
                can_rx_word_t rx;
                xQueueReceive(twai_rx_queue, &rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_START_CMD) {
                    xSemaphoreGive(ctrl_task_sem);
                    break;
                }
            }
        }

        else if (action == RX_RECEIVE_STOP_CMD) {
            while (1) {
                can_rx_word_t rx;
                xQueueReceive(twai_rx_queue, &rx, portMAX_DELAY);
                if (rx.id == ID_MASTER_STOP_CMD(SLAVE_ID)) {
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

/* ---------------- TX TASK ---------------- */

static void twai_transmit_task(void *arg)
{
    while (1) {
        tx_task_action_t action;
        xQueueReceive(tx_task_queue, &action, portMAX_DELAY);

        if (action == TX_SEND_PING_RESP) {
            twai_node_transmit(node_hdl,
                &(twai_frame_t){ .header.id = ID_SLAVE_PING_RESP }, portMAX_DELAY);
            xSemaphoreGive(ctrl_task_sem);
        }

        else if (action == TX_SEND_DATA) {
            while (1) {
                uint64_t payload =
                    ((uint64_t)xTaskGetTickCount()) |
                    ((uint64_t)(SLAVE_ID) << 32);

                uint8_t raw[8];
                memcpy(raw, &payload, 8);

                twai_frame_t tx = {
                    .header.id = ID_SLAVE_DATA,
                    .buffer = raw,
                    .buffer_len = 8,
                };

                twai_node_transmit(node_hdl, &tx, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(DATA_PERIOD_MS));

                if (xSemaphoreTake(stop_data_sem, 0) == pdTRUE)
                    break;
            }
        }

        else if (action == TX_SEND_STOP_RESP) {
            twai_node_transmit(node_hdl,
                &(twai_frame_t){ .header.id = ID_SLAVE_STOP_RESP }, portMAX_DELAY);
            xSemaphoreGive(ctrl_task_sem);
        }

        else if (action == TX_TASK_EXIT) {
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ---------------- CONTROL TASK ---------------- */

static void twai_control_task(void *arg)
{
    xSemaphoreTake(ctrl_task_sem, portMAX_DELAY);

    for (int iter = 0; iter < NO_OF_ITERS; iter++) {

        ESP_ERROR_CHECK(twai_node_enable(node_hdl));

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
        vTaskDelay(pdMS_TO_TICKS(ITER_DELAY_MS));
    }

    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

/* ---------------- app_main ---------------- */

void app_main(void)
{
    tx_task_queue = xQueueCreate(1, sizeof(tx_task_action_t));
    rx_task_queue = xQueueCreate(1, sizeof(rx_task_action_t));
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));

    ctrl_task_sem = xSemaphoreCreateBinary();
    stop_data_sem = xSemaphoreCreateBinary();
    done_sem = xSemaphoreCreateBinary();

    xTaskCreate(twai_receive_task, "TWAI_rx", 4096, NULL, RX_TASK_PRIO, NULL);
    xTaskCreate(twai_transmit_task, "TWAI_tx", 4096, NULL, TX_TASK_PRIO, NULL);
    xTaskCreate(twai_control_task, "TWAI_ctrl", 4096, NULL, CTRL_TSK_PRIO, NULL);

    twai_onchip_node_config_t cfg = {
        .io_cfg.tx = TX_GPIO_NUM,
        .io_cfg.rx = RX_GPIO_NUM,
        .bit_timing.bitrate = TRANSM_RATE,
        .tx_queue_depth = TX_QUEUE_DEPTH,
    };

    ESP_ERROR_CHECK(twai_new_node_onchip(&cfg, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(
        node_hdl, &(twai_event_callbacks_t){ .on_rx_done = twai_rx_cb }, NULL));

    xSemaphoreGive(ctrl_task_sem);
    xSemaphoreTake(done_sem, portMAX_DELAY);

    ESP_ERROR_CHECK(twai_node_delete(node_hdl));
}
