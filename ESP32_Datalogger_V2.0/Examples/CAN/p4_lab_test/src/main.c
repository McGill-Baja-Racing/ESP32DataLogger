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
#include <freertos/projdefs.h>



/* --------------------- Definitions and static variables ------------------ */


#define TX_GPIO_NUM             5               // CAN TX Pin
#define RX_GPIO_NUM             4               // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define EXAMPLE_TAG             "Master"
#define NODE_ID                 0               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       100 
#define TIME_BEACON_PERIOD_MS   100           

#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_MASTER_TIME_BEACON   0x0A2
    

#define ID_DATA                 (0x0B1 + NODE_ID)


// typedef enum {
//     TX_SEND_PINGS,
//     TX_SEND_START_CMD,
//     TX_SEND_STOP_CMD,
//     TX_TASK_EXIT,
// } tx_task_action_t;

// typedef enum {
//     RX_RECEIVE_PING_RESP,
//     RX_RECEIVE_DATA,
//     RX_RECEIVE_STOP_RESP,
//     RX_TASK_EXIT,
// } rx_task_action_t;

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

uint64_t data;

static QueueHandle_t twai_rx_queue;

static SemaphoreHandle_t sample_task_sem;
static SemaphoreHandle_t send_task_sem;

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

static const twai_frame_t start_message = {
    .header.id = ID_START_CMD,    // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = start_data,            // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

static const twai_frame_t stop_message = {
    .header.id = ID_STOP_CMD,     // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = stop_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};


// Receive from CAN ISR handler
twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};




/* --------------------------- Tasks and Functions -------------------------- */
static void receive_task(void *arg)
{
    /*
    This task will sample the needed data, buffer it (and add a timestamp if it has
    P4 timestamp) and wait for the data to be sent before sampling again.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    while (1)
    {
        can_rx_word_t rx_msg;
        xQueueReceive(twai_rx_queue, &rx_msg, portMAX_DELAY);
        if (1) {
            
            int32_t timestamp = (int32_t)( rx_msg.data >> 32    & 0xFFFFFFFF );
            int32_t value = (int32_t)((rx_msg.data) & 0xFFFFFFFF );

            ESP_LOGI(EXAMPLE_TAG,
                "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32 " | Value: %" PRIi32,
                rx_msg.id,
                timestamp,
                value);
        }
        
    }
    


}

static void time_beacon_task(void *arg)
{
    uint8_t raw[8];
 
    twai_frame_t beacon = {
        .header.id = ID_MASTER_TIME_BEACON,
        .header.ide = false,
        .buffer = raw,
        .buffer_len = 8,
    };
 
    while (1) {
        uint64_t t_main_us = (uint64_t)esp_timer_get_time();
 
        for (int i = 0; i < 8; i++) {
            raw[i] = (uint8_t)((t_main_us >> (8 * i)) & 0xFF);
        }

 
        // Send beacon
        (void)twai_node_transmit(node_hdl, &beacon, portMAX_DELAY);
        ESP_LOGI(EXAMPLE_TAG,
                         "TX Data | ts=%" PRIu64,
                         t_main_us);
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
 
    vTaskDelete(NULL);
}


void app_main(void)
{
    /* ---------- RTOS objects FIRST ---------- */
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    assert(twai_rx_queue);

    sample_task_sem = xSemaphoreCreateBinary();
    send_task_sem   = xSemaphoreCreateBinary();
    assert(sample_task_sem && send_task_sem);

    /* ---------- Install TWAI ---------- */
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(EXAMPLE_TAG, "TWAI node created");

    /* ---------- Register callbacks BEFORE enable ---------- */
    ESP_ERROR_CHECK(
        twai_node_register_event_callbacks(node_hdl, &cbs, NULL)
    );

    /* ---------- Enable / start TWAI ---------- */
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(EXAMPLE_TAG, "TWAI node enabled");

    /* ---------- Tasks LAST ---------- */
    //xTaskCreatePinnedToCore(time_beacon_task, "time_beacon", 4096, NULL, 8, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(receive_task,     "receive",     4096, NULL, 8, NULL, tskNO_AFFINITY);
}
