
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define TAG "logger"

#define SAMPLE_HZ          1000
#define SAMPLE_US          (1000000 / SAMPLE_HZ)

#define WRITE_PERIOD_MS    2000                     // How long each buffer records data for 
#define SAMPLE_SIZE_INT64  3                        // How many data values are in one sample (timestamp, id+dlc, data)

#define SAMPLES_PER_BLOCK  (WRITE_PERIOD_MS * SAMPLE_HZ / 1000)
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE_INT64 * sizeof(int64_t))

static int64_t bufferA[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT64];
static int64_t bufferB[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT64];

static volatile int activeBuffer = 0;
static volatile int sampleIndex = 0;

static SemaphoreHandle_t sample_sem;     // ISR → sample_task
static SemaphoreHandle_t block_ready_sem; // sample_task → sd_task
static SemaphoreHandle_t done_sem;

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static FILE *logFile = NULL;
static int writeCounter = 0;


/* --------------------- Definitions and static variables ------------------ */
//Example Configuration

#define TX_GPIO_NUM             5               // CAN TX Pin
#define RX_GPIO_NUM             4               // CAN RX Pin
#define TRANSM_RATE             1000000         // Bitrate bps
#define TX_QUEUE_DEPTH          5               // TX queue depth
#define RX_QUEUE_LENGTH         16              // RX queue depth
#define EXAMPLE_TAG             "TWAI Master"

#define ID_STOP_CMD      0x0A0
#define ID_START_CMD     0x0A1
#define ID_PING          0x0A2
// #define ID_SLAVE_STOP_RESP      0x0B0
// #define ID_SLAVE_DATA           0x0B1
// #define ID_SLAVE_PING_RESP      0x0B2

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
        msg.dlc = frame.buffer_len;
        msg.data = 0;

        // Pack into uint64_t (little-endian)
        // switch to big endian TODO
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


twai_node_handle_t node_hdl = nullptr;
twai_onchip_node_config_t node_config = {
    .io_cfg = {
        .tx = (gpio_num_t) TX_GPIO_NUM,             // TWAI TX GPIO pin
        .rx = (gpio_num_t) RX_GPIO_NUM,             // TWAI RX GPIO pin
    },
    .bit_timing = {
        .bitrate = TRANSM_RATE,        // bps bitrate
    },
    .tx_queue_depth = TX_QUEUE_DEPTH,  // Transmit queue depth set to 5
};


static const twai_frame_t start_message = {
    .header = {
        .id = ID_START_CMD,            // Message ID
        .ide = false,                  // Use 29-bit extended ID format
    },
    .buffer = start_data,              // Pointer to data to transmit
    .buffer_len = 0,                   // Length of data to transmit
};

static const twai_frame_t stop_message = {
    .header = {
        .id = ID_STOP_CMD,             // Message ID
        .ide = false,                  // Use 29-bit extended ID format
    },
    .buffer = stop_data,               // Pointer to data to transmit
    .buffer_len = 0,                   // Length of data to transmit
};


// Receive from CAN ISR handler
twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};






void app_main(void)
{
  ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

#if CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR50
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_SDR50;
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
#endif

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t cfg = {.ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID};
    sd_pwr_ctrl_handle_t pwr;
    if (sd_pwr_ctrl_new_on_chip_ldo(&cfg, &pwr) == ESP_OK)
        host.pwr_ctrl_handle = pwr;
#endif

    // SDMMC slot configuration
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = (gpio_num_t) 43;
    slot_config.cmd = (gpio_num_t) 44;
    slot_config.d0  = (gpio_num_t) 39;
    slot_config.d1  = (gpio_num_t) 40;
    slot_config.d2  = (gpio_num_t) 41;
    slot_config.d3  = (gpio_num_t) 42;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card");
        return;
    }

    sdmmc_card_print_info(stdout, card);


    //Create tasks, queues, and semaphores
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    
    

    //Install TWAI driver
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_LOGI(EXAMPLE_TAG, "Driver installed");
    
    // Handle CAN receive ISR
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));

}

// -------------------------------------------------------
// HARDWARE TIMER ISR (1 kHz)
// -------------------------------------------------------
void IRAM_ATTR sample_isr(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(sample_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}


// -------------------------------------------------------
// SAMPLE TASK (runs when ISR wakes it)
// -------------------------------------------------------
void sample_task(void *arg)
{
  // put into interupt task (the data into the buffers)
    while (1)
    {
        // Wait until ISR triggers
        xSemaphoreTake(sample_sem, portMAX_DELAY);

        // Create dummy sample (later replace with CAN message)
        //its here where I need to put the can message
        // I need to somehow extract it when it comes in and write it
        can_rx_word_t msg;
        if 
        (xQueueReceive(twai_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
          //msg, is whats extracted from the queue
          int64_t t = esp_timer_get_time() / 1000;

          portENTER_CRITICAL(&spinlock);
          
          if (sampleIndex < SAMPLES_PER_BLOCK)
          {
              // if (activeBuffer == 0)
              //     memcpy(bufferA[sampleIndex], sample, sizeof(sample));
              // else
              //     memcpy(bufferB[sampleIndex], sample, sizeof(sample));
              // Get pointer to current row in active buffer
              int64_t* row = (activeBuffer == 0) ? bufferA[sampleIndex] 
                                                  : bufferB[sampleIndex];
              
              // Write directly - clean and fast
              row[0] = t;
              row[1] = ((int64_t)msg.id << 8) | msg.dlc;
              row[2] = msg.data;
              
              sampleIndex++;
          }
        }
        

        if (sampleIndex >= SAMPLES_PER_BLOCK)
        {
            sampleIndex = SAMPLES_PER_BLOCK;
            // Tell SD writer task the block is ready
            xSemaphoreGive(block_ready_sem);
        }

        portEXIT_CRITICAL(&spinlock);
    }
}


// -------------------------------------------------------
// SD WRITER TASK (writes full buffers every 100 ms)
// -------------------------------------------------------

// Helper function to increment filename 
static void create_unique_log_filename(char *out_path, size_t max_len)
{
    int index = 1;

    while (1)
    {
        snprintf(out_path, max_len, "/sdcard/log_%04d.bin", index);

        struct stat st;
        if (stat(out_path, &st) != 0) {
            // File does NOT exist → safe to use
            return;
        }

        index++;
    }
}

void sd_writer_task(void *arg)
{
    char *log_path = (char*)arg;

    ESP_LOGI(TAG, "Creating log file: %s", log_path);

    logFile = fopen(log_path, "wb");
    if (!logFile) {
        ESP_LOGE(TAG, "Failed to open log file");
        vTaskDelete(NULL);
    }

    while (1)
    {
        xSemaphoreTake(block_ready_sem, portMAX_DELAY);

        portENTER_CRITICAL(&spinlock);
        int buf = activeBuffer;
        activeBuffer = !activeBuffer;
        sampleIndex = 0;
        portEXIT_CRITICAL(&spinlock);

        if (buf == 0)
            fwrite(bufferA, 1, BLOCK_SIZE_BYTES, logFile);
        else
            fwrite(bufferB, 1, BLOCK_SIZE_BYTES, logFile);

        fflush(logFile);
        writeCounter++;

        ESP_LOGI(TAG, "Wrote block %d", writeCounter);

        if (writeCounter >= 100) {
            fclose(logFile);
            ESP_LOGI(TAG, "Finished logging");
            xSemaphoreGive(done_sem);
            vTaskDelete(NULL);
        }
    }
}





