
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "../lib/can/frames.c"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "logger";

/* --------------------- Logging / buffer config ------------------ */
#define SAMPLE_SIZE_INT64  2
#define SAMPLES_PER_BLOCK  100
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE_INT64 * sizeof(int64_t))
#define MAX_BLOCK_WRITES   2
#define LOG_EVERY_N        50

#define ID_MASTER_TIME_BEACON   0x0A2
#define TIME_BEACON_PERIOD_MS   100   

static int64_t bufferA[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT64];
static int64_t bufferB[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT64];

static volatile int activeBuffer = 0;
static volatile int sampleIndex  = 0;

static SemaphoreHandle_t block_ready_sem;
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static FILE *logFile = NULL;
static int writeCounter = 0;

static volatile bool stop_logging = false;

static TaskHandle_t logger_task_handle = NULL;
static TaskHandle_t sd_task_handle     = NULL;

/* --------------------- TWAI config ------------------ */
#define TX_GPIO_NUM             5
#define RX_GPIO_NUM             4
#define TRANSM_RATE             1000000
#define TX_QUEUE_DEPTH          5
#define RX_QUEUE_LENGTH         256

/* --------------------- BTN config ------------------ */
#define CTRL_BTN_GPIO_NUM       12
#define ISR_BTN_QUEUE_LENGTH    2

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

static QueueHandle_t twai_rx_queue = NULL;
static twai_node_handle_t node_hdl = NULL;

static twai_onchip_node_config_t node_config = {
    .io_cfg = {
        .tx = (gpio_num_t)TX_GPIO_NUM,
        .rx = (gpio_num_t)RX_GPIO_NUM,
    },
    .bit_timing = {
        .bitrate = TRANSM_RATE,
    },
    .tx_queue_depth = TX_QUEUE_DEPTH,
};

static void create_unique_log_filename(char *out_path, size_t max_len)
{
    int index = 1;
    while (1) {
        snprintf(out_path, max_len, "/sdcard/log_%04d.bin", index);
        struct stat st;
        if (stat(out_path, &st) != 0) return;
        index++;
    }
}

/* ---------------------- Button Params -------------------- */
static QueueHandle_t isr_queue;
static volatile status = 0; // control state

static void IRAM_ATTR ctlr_btn_handler(void* arg) // IRAM_ATTR is to ensure code is placed in internal RAW
{
    int gpio_num = (int)arg;  // pin number passed as argument
    status = !status;
    xQueueSendFromISR(isr_queue, &status, NULL); // send to task
}
// Acts as interrupt for the button Press
void ctrl_btn_init(void){
    // NOTE: Please do not use the interrupt of GPIO36 and GPIO39 when using ADC or Wi-Fi and Bluetooth with sleep mode enabled. 
    // Look into circuit component to better understand rising edge
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << CTRL_BTN_GPIO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_LOGI("Btn","Config Activated");
}
/* --------------------- TWAI RX callback ------------------ */
static bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8] = {0};
    twai_frame_t frame = {};
    frame.buffer     = raw;
    frame.buffer_len = sizeof(raw);

    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {
        can_rx_word_t msg = {};
        msg.id = frame.header.id;

        uint8_t dlc = frame.header.dlc;
        if (dlc > 8) dlc = 8;
        msg.dlc = dlc;

        uint64_t packed = 0;
        for (int i = 0; i < msg.dlc; i++) {
            packed |= (uint64_t)raw[i] << (8 * i); // little-endian packing
        }
        msg.data = packed;

        (void)xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

static twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

/* --------------------- Tasks ------------------ */
static void logger_task(void *arg)
{
    (void)arg;
    uint32_t frame_count = 0;

    while (!stop_logging) {
        can_rx_word_t msg;

        if (xQueueReceive(twai_rx_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        frame_count++;

        uint32_t value32 = (uint32_t)(msg.data & 0xFFFFFFFFu);
        uint32_t ts32    = (uint32_t)((msg.data >> 32) & 0xFFFFFFFFu);

        ESP_LOGI(TAG,
                "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIi32 "| Value: %" PRIi32,
                msg.id,
                (uint32_t) ts32,
                (uint32_t)value32);

        bool block_full = false;
        int  used_buf   = 0;
        int  used_idx   = -1;

        portENTER_CRITICAL(&spinlock);
        {
            if (sampleIndex < SAMPLES_PER_BLOCK) {
                int64_t *row = (activeBuffer == 0) ? bufferA[sampleIndex] : bufferB[sampleIndex];

                row[0] = (int64_t)msg.id;
                row[1] = (int64_t)msg.data;

                used_buf = activeBuffer;
                used_idx = sampleIndex;

                sampleIndex++;
                if (sampleIndex >= SAMPLES_PER_BLOCK) {
                    block_full = true;
                    sampleIndex = SAMPLES_PER_BLOCK;
                }
            } else {
                used_buf = activeBuffer;
            }
        }
        portEXIT_CRITICAL(&spinlock);

        if ((LOG_EVERY_N > 0) && ((frame_count % LOG_EVERY_N) == 0) && used_idx >= 0) {
            ESP_LOGI(TAG, "BUF%c[%d] <- ID=0x%03" PRIx32 " ts=%" PRIu32 " val=%" PRIu32 " data=0x%016" PRIx64,
                     used_buf ? 'B' : 'A', used_idx, msg.id, ts32, value32, msg.data);
        }

        if (block_full) {
            ESP_LOGW(TAG, "Block full: BUF%c ready for SD write (frames=%u)",
                     used_buf ? 'B' : 'A', frame_count);
            sampleIndex   = 0;
            xSemaphoreGive(block_ready_sem);
        }
    }

    ESP_LOGI(TAG, "logger_task exiting");
    vTaskDelete(NULL);
}

static void sd_writer_task(void *arg)
{
    const char *log_path = (const char *)arg;

    ESP_LOGI(TAG, "Creating log file: %s", log_path);
    logFile = fopen(log_path, "wb");
    if (!logFile) {
        ESP_LOGE(TAG, "Failed to open log file");
        stop_logging = true;
        vTaskDelete(NULL);
        return;
    }

    while (!stop_logging) { 
        xSemaphoreTake(block_ready_sem, portMAX_DELAY);

        int buf_to_write;

        portENTER_CRITICAL(&spinlock);
        {
            buf_to_write  = activeBuffer;
            activeBuffer  = !activeBuffer;
        }
        portEXIT_CRITICAL(&spinlock);

        ESP_LOGI(TAG, "Switched active buffer -> BUF%c (writing BUF%c)",
                 activeBuffer ? 'B' : 'A', buf_to_write ? 'B' : 'A');

        const void *ptr = (buf_to_write == 0) ? (const void *)bufferA : (const void *)bufferB;

        ESP_LOGI(TAG, "Writing BUF%c to SD (%u bytes)...",
                 buf_to_write ? 'B' : 'A', (unsigned)BLOCK_SIZE_BYTES);

        size_t written = fwrite(ptr, 1, BLOCK_SIZE_BYTES, logFile);
        fflush(logFile);
        writeCounter++;

        if (written != BLOCK_SIZE_BYTES) {
            ESP_LOGE(TAG, "Short write! wrote=%u expected=%u",
                     (unsigned)written, (unsigned)BLOCK_SIZE_BYTES);
        } else {
            ESP_LOGI(TAG, "Wrote block %d / %d", writeCounter, MAX_BLOCK_WRITES);
        }

        if (writeCounter >= MAX_BLOCK_WRITES) {
            ESP_LOGW(TAG, "Reached %d block writes. Stopping logging...", MAX_BLOCK_WRITES);
            stop_logging = true;

            fclose(logFile);
            logFile = NULL;
            ESP_LOGI(TAG, "Log file closed.");

            if (logger_task_handle) {
                vTaskDelete(logger_task_handle);
                logger_task_handle = NULL;
            }

            ESP_LOGI(TAG, "sd_writer_task exiting");
            vTaskDelete(NULL);
            return;
        }
    }

    if (logFile) {
        fclose(logFile);
        logFile = NULL;
        ESP_LOGI(TAG, "Log file closed (stop_logging).");
    }

    ESP_LOGI(TAG, "sd_writer_task exiting");
    vTaskDelete(NULL);
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
        ESP_LOGI(TAG,
                         "TX Data | ts=%" PRIu64,
                         t_main_us);
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
 
    vTaskDelete(NULL);
}
/* --------------------- Button Control ------------------ */


/* ------------------- Start and Stop ---------------------- */
void v_send_control_cmd(void *args ){
    while(1){
        if (xQueueReceive(isr_queue, &status, portMAX_DELAY)) {
            ESP_LOGI("btn-Task", "ISR Received");
            control_msg_data[0] = (control_cmd_t)status;
            ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &control_message, 0));  // Timeout = 0: returns immediately if queue is full
            ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));  // Wait for transmission to finish
        }
    }
}

/* --------------------- app_main ------------------ */
void app_main(void)
{   
    ESP_LOGI(TAG, "Control Button Initialisation");
    ctrl_btn_init();
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CTRL_BTN_GPIO_NUM,ctlr_btn_handler,(void*)status);

    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // Power SD IO via on-chip LDO channel 4
    sd_pwr_ctrl_ldo_config_t cfg = {};
    cfg.ldo_chan_id = 4; // CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID=4

    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&cfg, &pwr);
    if (pwr_err == ESP_OK) {
        host.pwr_ctrl_handle = pwr;
        ESP_LOGI(TAG, "SD IO power control enabled (on-chip LDO chan %d)", cfg.ldo_chan_id);
    } else {
        ESP_LOGW(TAG, "SD IO power control NOT enabled: %s", esp_err_to_name(pwr_err));
    }
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    slot_config.clk = (gpio_num_t)43;
    slot_config.cmd = (gpio_num_t)44;
    slot_config.d0  = (gpio_num_t)39;
    slot_config.d1  = (gpio_num_t)40;
    slot_config.d2  = (gpio_num_t)41;
    slot_config.d3  = (gpio_num_t)42;

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Avoid “phantom” CD/WP defaults
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;

    //give the card a moment after boot (often helps OCR timeouts)
    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_card_print_info(stdout, card);

    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI RX queue");
        return;
    }

    block_ready_sem = xSemaphoreCreateCounting(2, 0);
    if (!block_ready_sem) {
        ESP_LOGE(TAG, "Failed to create block_ready_sem");
        return;
    }

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled");

    static char log_path[64];
    create_unique_log_filename(log_path, sizeof(log_path));

    xTaskCreate(sd_writer_task, "sd_writer", 8192, (void*)log_path, 10, &sd_task_handle);
    xTaskCreate(logger_task,    "logger",    4096, NULL,        9,  &logger_task_handle);
    xTaskCreatePinnedToCore(time_beacon_task, "time_beacon", 4096, NULL, 8, NULL, tskNO_AFFINITY);

    ESP_LOGI(TAG, "Data logger started.");
}


