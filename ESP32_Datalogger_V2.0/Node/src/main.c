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

#include "driver/adc.h"
#include "esp_adc_cal.h"




/* --------------------- Definitions and static variables ------------------ */


#define TX_GPIO_NUM             21              // CAN TX Pin
#define RX_GPIO_NUM             20              // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define TAG             "Node"
#define NODE_ID                 2               // CHANGE TO NODE ID
#define SAMPLING_SPEED_MS       1           
// ID1 = brakes
// ID2 = wheel
// ID0 = engine


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

// -------------------- ADC CONFIG (CHANGE THESE) --------------------
// Pick the ADC unit + channel that corresponds to your chosen GPIO.
// You MUST confirm the mapping for your ESP32-P4 board/pinout.
static const adc_unit_t    ADC_UNIT_USED = ADC_UNIT_1;
static const adc_channel_t ADC_CH_USED   = 0;   // <-- CHANGE
static const adc_atten_t   ADC_ATTEN_USED = ADC_ATTEN_DB_12;

#define ADC_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP

static esp_adc_cal_characteristics_t adc1_chars;
static int adc_raw[2][10];

// -------------------------------------------------------
// ADC init + optional calibration
// -------------------------------------------------------
static bool adc_calibration_init(void)
{
    esp_err_t ret;
    bool cali_enable = false;

    ret = esp_adc_cal_check_efuse(ADC_CALI_SCHEME);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Calibration scheme not supported, skip software calibration");
    } else if (ret == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else if (ret == ESP_OK) {
        cali_enable = true;
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_USED, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
    } else {
        ESP_LOGE(TAG, "Invalid arg");
    }

    return cali_enable;
}


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

static volatile int64_t g_offset_us;

static inline void handle_time_beacon(const can_rx_word_t *rx){
    uint64_t t_main_us = rx ->data;
    int64_t t_local_us = esp_timer_get_time();

    g_offset_us = (int64_t)t_main_us - t_local_us;
}


/* --------------------------- Tasks and Functions -------------------------- */
static void sample_task(void *arg)
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
        xSemaphoreTake(sample_task_sem, portMAX_DELAY);

        int64_t t_local_sample_us = esp_timer_get_time();
        int64_t t_main_sample_us = t_local_sample_us + g_offset_us;
        int32_t timestamp = (int32_t)t_main_sample_us/1000 ; // Convert it from us to ms
        
        int32_t value = timestamp + 100;   // example payload

        data =
                ((uint64_t)(uint32_t)timestamp << 32)
            | ((uint64_t)(uint32_t)value );

        // allow sample task to run
        xSemaphoreGive(send_task_sem);
        
    }
}

static void sample_adc_task(void *arg)
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
        xSemaphoreTake(sample_task_sem, portMAX_DELAY);

         // Read ADC (raw)
        int raw = 0;
        adc_raw[0][0] = adc1_get_raw(ADC_CH_USED);
        ESP_LOGI(TAG, "raw  data: %d", adc_raw[0][0]);
        raw = adc_raw[0][0];


        int64_t t_local_sample_us = esp_timer_get_time();
        int64_t t_main_sample_us = t_local_sample_us + g_offset_us;
        int32_t timestamp = (int32_t)t_main_sample_us/1000 ; // Convert it from us to ms
        
        int32_t value = (int32_t) raw;

        data =
                ((uint64_t)(uint32_t)timestamp << 32)
            | ((uint64_t)(uint32_t)value );

        // allow sample task to run
        xSemaphoreGive(send_task_sem);
        
    }
}

static void sample_rpm_task(void *arg)
{
    /*
    This task will sample the ADC pulse, detect the peak, buffer the data (timestamp and ADC value),
    and send it once for each pulse detected.
    */

    bool peak_detected = false;
    int raw = 0;
    
    while (1)
    {
        // Wait for signal to start sampling
        //xSemaphoreTake(sample_task_sem, portMAX_DELAY);

        // Read ADC (raw)

        raw = adc1_get_raw(ADC_CH_USED);
        

        // Peak detection - Check for the rising edge or threshold crossing
        if (raw < 1000 && !peak_detected) {
            peak_detected = true;

            // Capture the timestamp in microseconds
            int64_t t_local_sample_us = esp_timer_get_time();
            int64_t t_main_sample_us = t_local_sample_us + g_offset_us;

            // Optionally, you can keep timestamp in microseconds for better precision
            int32_t timestamp = (int32_t)(t_main_sample_us / 1000); // Convert from us to ms
            
            int32_t value = (int32_t)raw;

            // Pack timestamp and value into the data structure
            data =
                ((uint64_t)(uint32_t)timestamp << 32)
            | ((uint64_t)(uint32_t)value );


            ESP_LOGI(TAG, "raw  data: %d", raw);

            
            // Allow sample task to run
            xSemaphoreGive(send_task_sem);
        }
        // Reset peak detection after sending data
        else if (raw > 1000 && peak_detected) {
            peak_detected = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}


static void send_task(void *arg)
{
    /*
    Send the data over CAN every SAMPLING_SPEED_MS 

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    while (1)
    {

        // Wait until data is ready
        xSemaphoreTake(send_task_sem, portMAX_DELAY);

        uint8_t raw[8];
        // Use memcpy to go from uint64 to array
        memcpy(raw, &data, 8);

        twai_frame_t tx = {
            .header.id = ID_DATA,
            .header.ide = false,
            .buffer = raw,
            .buffer_len = 8,
        };

        // Send data
        twai_node_transmit(node_hdl, &tx, portMAX_DELAY);

        int32_t value = (int32_t)( data    & 0xFFFFFFFF );
        int32_t timestamp = (int32_t)((data >> 32) & 0xFFFFFFFF );
         ESP_LOGI(TAG,
                         "TX Data | ts=%" PRIi32 " val=%" PRIi32,
                         timestamp, value);

        //vTaskDelay(pdMS_TO_TICKS(SAMPLING_SPEED_MS));


    }

}

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
        if (rx_msg.id == ID_MASTER_TIME_BEACON) {
            
            handle_time_beacon(&rx_msg);
            ESP_LOGI(TAG,
                "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32,
                rx_msg.id,
                (uint32_t)rx_msg.data);

        }
        
        
    }
    


}


void app_main(void)
{
    uint32_t voltage = 0;
    bool cali_enable = adc_calibration_init();

    //ADC1 config
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC_CH_USED , ADC_ATTEN_USED));

    //Create tasks, queues, and semaphores
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    sample_task_sem = xSemaphoreCreateBinary();
    send_task_sem = xSemaphoreCreateBinary();

    //Install TWAI driver
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(TAG, "Driver installed");
    
    // Handle CAN receive ISR
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

    /* ---------- Enable / start TWAI ---------- */
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI node enabled");

    xTaskCreatePinnedToCore(send_task, "send", 8192, NULL, 7, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(sample_rpm_task, "sample", 8192, NULL, 8, NULL, tskNO_AFFINITY);
    //xTaskCreatePinnedToCore(receive_task, "receive", 4096, NULL, 8, NULL, tskNO_AFFINITY);
}