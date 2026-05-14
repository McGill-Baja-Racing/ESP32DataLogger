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
#define PULSES_PER_ROTATION     1               // Set this to the number of pulses in one full rotation

// -------------------- TASK RATE CONFIG --------------------
// Set SEND_PERIOD_MS to 0 to transmit queued samples as fast as TWAI accepts them.
#define SAMPLE_QUEUE_LENGTH     64
#define EXAMPLE_SAMPLE_PERIOD_MS 100
#define ADC_SAMPLE_PERIOD_MS    1
#define RPM_SAMPLE_PERIOD_MS    2
#define SEND_PERIOD_MS          0

#define ENABLE_EXAMPLE_SAMPLE_TASK 1
#define ENABLE_ADC_SAMPLE_TASK     0
#define ENABLE_RPM_SAMPLE_TASK     0
// ID1 = brakes
// ID2 = wheel
// ID0 = engine


#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_MASTER_TIME_BEACON   0x0A2
#define ID_NODE_STATE           (0x0C0 + NODE_ID)
#define NODE_STATE_LOW_POWER    0
#define NODE_STATE_ACTIVE       1

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

typedef struct {
    int32_t timestamp_ms;
    int32_t value;
} sample_data_t;

static QueueHandle_t twai_rx_queue;
static QueueHandle_t sample_tx_queue;
static volatile bool s_node_active = false;

// -------------------- ADC CONFIG (CHANGE THESE) --------------------
// Pick the ADC unit + channel that corresponds to your chosen GPIO.
// ESP32-C3 ADC1 channels map to GPIO0-GPIO4.
static const adc_unit_t    ADC_UNIT_USED = ADC_UNIT_1;
static const adc_channel_t ADC_CH_USED   = ADC_CHANNEL_4;   
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
        esp_adc_cal_characterize(ADC_UNIT_USED, ADC_ATTEN_USED, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
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

static inline int32_t get_synced_timestamp_ms(void)
{
    int64_t t_local_sample_us = esp_timer_get_time();
    int64_t t_main_sample_us = t_local_sample_us + g_offset_us;

    return (int32_t)(t_main_sample_us / 1000);
}

static bool queue_sample(int32_t timestamp_ms, int32_t value)
{
    if (!s_node_active) {
        return false;
    }

    sample_data_t sample = {
        .timestamp_ms = timestamp_ms,
        .value = value,
    };

    if (xQueueSend(sample_tx_queue, &sample, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Sample TX queue send failed | ts=%" PRIi32 " val=%" PRIi32,
                 timestamp_ms, value);
        return false;
    }

    return true;
}

static uint64_t pack_sample_data(const sample_data_t *sample)
{
    return ((uint64_t)(uint32_t)sample->timestamp_ms << 32)
         | ((uint64_t)(uint32_t)sample->value);
}

static void send_node_state(uint8_t state)
{
    uint8_t raw[1] = {state};
    twai_frame_t tx = {
        .header.id = ID_NODE_STATE,
        .header.ide = false,
        .buffer = raw,
        .buffer_len = sizeof(raw),
    };

    esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TX STATE | node=%d state=%s",
                 NODE_ID,
                 state == NODE_STATE_ACTIVE ? "active" : "low-power");
    } else {
        ESP_LOGW(TAG, "TX STATE failed | node=%d state=%u err=%s",
                 NODE_ID, state, esp_err_to_name(err));
    }
}

static void wait_until_node_active(void)
{
    while (!s_node_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/* --------------------------- Tasks and Functions -------------------------- */
static void sample_task(void *arg)
{
    /*
    Example sampler. Each sample task pushes timestamped values into sample_tx_queue.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    while (1)
    {
        wait_until_node_active();
        TickType_t last_wake = xTaskGetTickCount();

        while (s_node_active) {
            int32_t timestamp = get_synced_timestamp_ms();
            int32_t value = timestamp + 100;   // example payload

            queue_sample(timestamp, value);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(EXAMPLE_SAMPLE_PERIOD_MS));
        }
    }
}

static void sample_adc_task(void *arg)
{
    /*
    This task samples the ADC and pushes timestamped pressure values into
    sample_tx_queue.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    while (1)
    {
        wait_until_node_active();
        TickType_t last_wake = xTaskGetTickCount();

        while (s_node_active) {
            int raw = 0;
            adc_raw[0][0] = adc1_get_raw(ADC_CH_USED);
            raw = adc_raw[0][0];

            int32_t timestamp = get_synced_timestamp_ms();
            float value = (float) raw * 2.5 / 4095.0 + 0.18;
            int32_t value_mv = (int32_t)(value * 1000.0f);

            queue_sample(timestamp, value_mv);
            ESP_LOGI(TAG,
                        "pressure=%.2f raw=%d",
                        value, raw);

            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS));
        }
    }
}

static void sample_rpm_task(void *arg)
{
    /*
    This task samples the ADC pulse, detects each pulse crossing, and computes RPM
    from the time between the current pulse and the previous one.
    */

    while (1)
    {
        wait_until_node_active();

        bool peak_detected = false;
        int64_t last_pulse_us = 0;
        int64_t last_log_us = 0;
        int raw = 0;
        TickType_t last_wake = xTaskGetTickCount();

        while (s_node_active) {
            raw = adc1_get_raw(ADC_CH_USED);
            

            // Detect one pulse when the signal crosses below the threshold.
            if (raw < 1000 && !peak_detected) {
                peak_detected = true;

                int64_t t_main_sample_us = esp_timer_get_time() + g_offset_us;
                int32_t timestamp = (int32_t)(t_main_sample_us / 1000); // Convert from us to ms
                int64_t delta_us = 0;
                int32_t rpm = 0;

                if (last_pulse_us != 0) {
                    delta_us = t_main_sample_us - last_pulse_us;
                    if (delta_us > 0) {
                        rpm = (int32_t)(60000000LL / (delta_us * PULSES_PER_ROTATION));
                    }
                }

                last_pulse_us = t_main_sample_us;

                queue_sample(timestamp, rpm);

                if (delta_us > 0 && (t_main_sample_us - last_log_us) >= 100000) {
                    ESP_LOGI(TAG, "Pulse detected | raw=%d dt=%lld us rpm=%ld",
                            raw, delta_us, (long)rpm);
                    last_log_us = t_main_sample_us;
                } else if (delta_us == 0) {
                    ESP_LOGI(TAG, "First pulse detected | raw=%d waiting for next pulse", raw);
                }

                
            }
            // Use a higher reset threshold to avoid retriggering on noisy edges.
            else if (raw > 1200 && peak_detected) {
                peak_detected = false;
            }

            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(RPM_SAMPLE_PERIOD_MS));

        }
    }
}


static void send_task(void *arg)
{
    /*
    Sends queued samples over CAN. Sampling and sending are decoupled so samplers
    can continue collecting while the bus is busy.

    Data is packaged as:
        - 1st 32 bits: Timestamp
        - 2nd 32 bits: Value
    */
    TickType_t last_send = xTaskGetTickCount();

    while (1)
    {
        sample_data_t sample;
        xQueueReceive(sample_tx_queue, &sample, portMAX_DELAY);

        if (!s_node_active) {
            continue;
        }

        uint8_t raw[8];
        uint64_t data = pack_sample_data(&sample);
        memcpy(raw, &data, 8);

        twai_frame_t tx = {
            .header.id = ID_DATA,
            .header.ide = false,
            .buffer = raw,
            .buffer_len = 8,
        };

        // Send data
        ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &tx, portMAX_DELAY));

         ESP_LOGI(TAG,
                         "TX Data | ts=%" PRIi32 " val=%" PRIi32,
                         sample.timestamp_ms, sample.value);

        if (SEND_PERIOD_MS > 0) {
            vTaskDelayUntil(&last_send, pdMS_TO_TICKS(SEND_PERIOD_MS));
        }


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
        else if (rx_msg.id == ID_START_CMD) {
            if (!s_node_active) {
                xQueueReset(sample_tx_queue);
                s_node_active = true;
                ESP_LOGI(TAG, "START received - sampling enabled");
                send_node_state(NODE_STATE_ACTIVE);
            }
        }
        else if (rx_msg.id == ID_STOP_CMD) {
            if (s_node_active) {
                s_node_active = false;
                xQueueReset(sample_tx_queue);
                ESP_LOGI(TAG, "STOP received - sampling disabled");
                send_node_state(NODE_STATE_LOW_POWER);
            } else {
                send_node_state(NODE_STATE_LOW_POWER);
            }
        }
        
        
    }
    


}

void app_main(void)
{
    bool cali_enable = adc_calibration_init();
    (void)cali_enable;

    //ADC1 config
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC_CH_USED , ADC_ATTEN_USED));

    //Create tasks, queues, and semaphores
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    sample_tx_queue = xQueueCreate(SAMPLE_QUEUE_LENGTH, sizeof(sample_data_t));
    ESP_ERROR_CHECK(twai_rx_queue == NULL ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(sample_tx_queue == NULL ? ESP_FAIL : ESP_OK);

    //Install TWAI driver
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(TAG, "Driver installed");
    
    // Handle CAN receive ISR
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

    /* ---------- Enable / start TWAI ---------- */
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI node enabled; waiting for START command");
    send_node_state(NODE_STATE_LOW_POWER);

    xTaskCreatePinnedToCore(send_task, "send", 8192, NULL, 7, NULL, tskNO_AFFINITY);
#if ENABLE_EXAMPLE_SAMPLE_TASK
    xTaskCreatePinnedToCore(sample_task, "sample", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
#if ENABLE_ADC_SAMPLE_TASK
    xTaskCreatePinnedToCore(sample_adc_task, "sample_adc", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
#if ENABLE_RPM_SAMPLE_TASK
    xTaskCreatePinnedToCore(sample_rpm_task, "sample_rpm", 8192, NULL, 2, NULL, tskNO_AFFINITY);
#endif
    xTaskCreatePinnedToCore(receive_task, "receive", 4096, NULL, 8, NULL, tskNO_AFFINITY);
}
