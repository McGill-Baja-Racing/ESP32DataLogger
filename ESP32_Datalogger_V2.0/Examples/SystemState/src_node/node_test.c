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

#include "../lib/can/frames.c"



/* --------------------- Definitions and static variables ------------------ */


#define TX_GPIO_NUM             20              // CAN TX Pin
#define RX_GPIO_NUM             21              // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define EXAMPLE_TAG             "Node"
#define NODE_ID                 0               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       100 

#define ID_DATA                 (0x0B1 + NODE_ID)


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
    // Start the TWAI controller
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));

    ESP_ERROR_CHECK(twai_node_disable(node_hdl));
    // ESP_ERROR_CHECK must alwasy be insde a scope { }
}



void app_main(void)
{
    
    init_node();
}
/*
twai_mask_filter_config_t mfilter_cfg = {
    .id = 0x10,         // 0b 000 0001 0000
    .mask = 0x7f0,      // 0b 111 1111 0000 — the upper 7 bits must match strictly, the lower 4 bits are ignored, accepts IDs of the form
                        // 0b 000 0001 xxxx (hex 0x01x)
    .is_ext = false,    // Accept only standard IDs, not extended IDs
};
ESP_ERROR_CHECK(twai_node_config_mask_filter(node_hdl, 0, &mfilter_cfg))*/