// ---- ISR-DRIVEN SD logging 1 kHz Datalogger for ESP32-P4 ----
// Timer ISR → wakes sample_task
// sample_task reads ADC + fills buffer
// SD writer task writes every 2 seconds
// Stops after 100 SD writes
//
// NOTE: You MUST set ADC_UNIT / ADC_CHANNEL to match the GPIO you actually wired.
//       (ADC channel <-> GPIO mapping is chip/board-specific.)

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_test_io.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define TAG "logger"

#define SAMPLE_HZ          1000
#define SAMPLE_US          (1000000 / SAMPLE_HZ)

#define WRITE_PERIOD_MS    2000                     // How long each buffer records data for
#define SAMPLE_SIZE_INT32  5                        // How many int32 values are in one sample

#define SAMPLES_PER_BLOCK  (WRITE_PERIOD_MS * SAMPLE_HZ / 1000)
#define BLOCK_SIZE_BYTES   (SAMPLES_PER_BLOCK * SAMPLE_SIZE_INT32 * sizeof(int32_t))

static int32_t bufferA[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT32];
static int32_t bufferB[SAMPLES_PER_BLOCK][SAMPLE_SIZE_INT32];

static volatile int activeBuffer = 0;
static volatile int sampleIndex  = 0;

static SemaphoreHandle_t sample_sem;       // ISR → sample_task
static SemaphoreHandle_t block_ready_sem;  // sample_task → sd_task
static SemaphoreHandle_t done_sem;

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static FILE *logFile = NULL;
static int writeCounter = 0;

// -------------------- ADC CONFIG (CHANGE THESE) --------------------
// Pick the ADC unit + channel that corresponds to your chosen GPIO.
// You MUST confirm the mapping for your ESP32-P4 board/pinout.
static const adc_unit_t    ADC_UNIT_USED = ADC_UNIT_1;
static const adc_channel_t ADC_CH_USED   = 4;   // <-- CHANGE
static const adc_atten_t   ADC_ATTEN_USED = ADC_ATTEN_DB_11;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_cali_enabled = false;

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
// ADC init + optional calibration
// -------------------------------------------------------
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_CH_USED, &chan_cfg));

    // Optional calibration (if supported by your IDF + target)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_USED,
        .chan = ADC_CH_USED,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        ESP_LOGW(TAG, "ADC calibration not available; logging raw ADC counts only");
    }
#else
    ESP_LOGW(TAG, "ADC calibration scheme not supported; logging raw ADC counts only");
#endif
}

// -------------------------------------------------------
// SAMPLE TASK (runs when ISR wakes it)
// -------------------------------------------------------
void sample_task(void *arg)
{
    (void)arg;

    while (1)
    {
        // Wait until ISR triggers
        xSemaphoreTake(sample_sem, portMAX_DELAY);

        // Read ADC (raw)
        int raw = 0;
        if (s_adc_handle) {
            (void)adc_oneshot_read(s_adc_handle, ADC_CH_USED, &raw);
        }

        // Convert to mV if calibration is enabled; else keep mV = -1
        int mv = -1;
        if (s_cali_enabled && s_cali_handle) {
            (void)adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        }

        // Build sample payload (5 int32 values)
        // You can reorder these however you want.
        int32_t t_ms = (int32_t)(esp_timer_get_time() / 1000);

        int32_t sample[SAMPLE_SIZE_INT32] = {
            t_ms,            // [0] timestamp (ms)
            (int32_t)raw,    // [1] ADC raw counts
            (int32_t)mv,     // [2] ADC millivolts (or -1 if not calibrated)
            0,               // [3] spare (put other signals here later)
            0                // [4] spare
        };

        portENTER_CRITICAL(&spinlock);

        if (sampleIndex < SAMPLES_PER_BLOCK)
        {
            if (activeBuffer == 0)
                memcpy(bufferA[sampleIndex], sample, sizeof(sample));
            else
                memcpy(bufferB[sampleIndex], sample, sizeof(sample));

            sampleIndex++;
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
// SD WRITER TASK
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

        if (writeCounter >= 3) {
            fclose(logFile);
            ESP_LOGI(TAG, "Finished logging");
            xSemaphoreGive(done_sem);
            vTaskDelete(NULL);
        }
    }
}

// -------------------------------------------------------
// MAIN
// -------------------------------------------------------
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
    slot_config.clk = 43;
    slot_config.cmd = 44;
    slot_config.d0  = 39;
    slot_config.d1  = 40;
    slot_config.d2  = 41;
    slot_config.d3  = 42;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card");
        return;
    }

    sdmmc_card_print_info(stdout, card);

    // Init ADC
    adc_init();

    // Create semaphores
    sample_sem = xSemaphoreCreateBinary();
    block_ready_sem = xSemaphoreCreateBinary();
    done_sem = xSemaphoreCreateBinary();

    // Get unique log file name
    char log_path_global[64];
    create_unique_log_filename(log_path_global, sizeof(log_path_global));

    // Start tasks
    xTaskCreatePinnedToCore(sd_writer_task, "sd_task", 4096, strdup(log_path_global), 4, NULL, 1);
    xTaskCreatePinnedToCore(sample_task,    "sample_task", 4096, NULL, 5, NULL, 0);

    // Create and start 1 kHz timer ISR
    const esp_timer_create_args_t timer_args = {
        .callback = sample_isr,
        .arg = NULL,
        .name = "sampler_isr"
    };

    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, SAMPLE_US));

    // Wait until datalogging ends
    xSemaphoreTake(done_sem, portMAX_DELAY);

    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    ESP_LOGI(TAG, "SD unmounted, datalogging complete.");
}


