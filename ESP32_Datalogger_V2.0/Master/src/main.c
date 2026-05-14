
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "Master";

/* --------------------- CONTSTANT VARIABLES ------------------ */
#define INFO_PER_SAMPLE  2          // CAN ID, Data (Timestamp + value)
#define SAMPLES_PER_BLOCK  100
#define LOG_SAVE_INTERVAL_MS 15000

// ID
#define ID_MASTER_TIME_BEACON   0x0A2
#define ID_NODE1_HEARTBEAT      0x000 // TO DO SET THIS TO A GOOD VALUE
#define ID_STOP_CMD             0x0A0
#define ID_START_CMD            0x0A1
#define ID_NODE_STATE_BASE      0x0C0
#define NODE_STATE_LOW_POWER    0
#define NODE_STATE_ACTIVE       1
#define NODE_LOW_POWER_ACK_WAIT_MS 1000

#define TIME_BEACON_PERIOD_MS   100   
#define LOG_PATH_MAX_LEN        64
#define MAX_NODE_SENSORS        4

static SemaphoreHandle_t log_file_mutex = NULL;

static FILE *logFile = NULL;
static int writeCounter = 0;

typedef enum {
    LOG_STATE_IDLE = 0,
    LOG_STATE_RUNNING,
    LOG_STATE_STOPPING,
} log_state_t;

static volatile log_state_t log_state = LOG_STATE_IDLE;
static char current_log_path[LOG_PATH_MAX_LEN];

static TaskHandle_t dispatch_task_handle = NULL;
static TaskHandle_t sd_task_handle     = NULL;
static TaskHandle_t save_task_handle   = NULL;
static TaskHandle_t serial_task_handle = NULL;

/* --------------------- TWAI/CAN config ------------------ */
#define TX_GPIO_NUM             20
#define RX_GPIO_NUM             21
#define TRANSM_RATE             1000000
#define TX_QUEUE_DEPTH          5
#define RX_QUEUE_LENGTH         256

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

typedef struct {
    uint8_t sensor_id;
    const char *name;
    uint32_t can_id;
    const char *units;
} sensor_config_t;

typedef struct {
    uint8_t node_id;
    const char *name;
    uint32_t state_can_id;
    bool active;
    bool low_power_ack_seen;
    int64_t last_state_rx_us;
    sensor_config_t sensors[MAX_NODE_SENSORS];
    size_t sensor_count;
} node_config_entry_t;

static node_config_entry_t configured_nodes[] = {
    {
        .node_id = 2,
        .name = "sensor_node_2",
        .state_can_id = ID_NODE_STATE_BASE + 2,
        .active = false,
        .low_power_ack_seen = false,
        .last_state_rx_us = 0,
        .sensors = {
            {.sensor_id = 2, .name = "example_timestamp_value", .can_id = 0x0B1 + 2, .units = "raw"},
        },
        .sensor_count = 1,
    },
};

static const size_t configured_node_count =
    sizeof(configured_nodes) / sizeof(configured_nodes[0]);

static QueueHandle_t twai_rx_queue = NULL;
static QueueHandle_t twai_rx_heartbeat_queue = NULL;
static QueueHandle_t twai_rx_sample_queue = NULL;
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

/* --------------------- TWAI RX ------------------ */
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
            packed |= (uint64_t)raw[i] << (8 * i); 
        }
        msg.data = packed;

        (void)xQueueSendFromISR(twai_rx_queue, &msg, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

static twai_event_callbacks_t cbs = {
    .on_rx_done = twai_rx_cb,
};

// Finds unique filename to write new data too. File name format is LOG_XXXX.bin
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

static void close_log_file_if_open(void)
{
    if (logFile) {
        fflush(logFile);
        fclose(logFile);
        logFile = NULL;
        ESP_LOGI(TAG, "Log file closed.");
    }
}

static bool reopen_log_file_append(const char *path)
{
    FILE *reopened = fopen(path, "ab");
    if (!reopened) {
        ESP_LOGE(TAG, "Failed to reopen log file: %s", path);
        return false;
    }

    logFile = reopened;
    ESP_LOGI(TAG, "Log file reopened.");
    return true;
}

static const char *log_state_name(log_state_t state)
{
    switch (state) {
    case LOG_STATE_IDLE:
        return "idle";
    case LOG_STATE_RUNNING:
        return "running";
    case LOG_STATE_STOPPING:
        return "stopping";
    default:
        return "unknown";
    }
}

static esp_err_t send_control_command(uint32_t id)
{
    twai_frame_t tx = {
        .header.id = id,
        .header.ide = false,
        .buffer = NULL,
        .buffer_len = 0,
    };

    esp_err_t err = twai_node_transmit(node_hdl, &tx, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TX CMD | ID: 0x%03" PRIX32, id);
    } else {
        ESP_LOGW(TAG, "TX CMD failed | ID: 0x%03" PRIX32 " err=%s", id, esp_err_to_name(err));
    }
    return err;
}

static node_config_entry_t *find_configured_node_by_state_can_id(uint32_t can_id)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        if (configured_nodes[i].state_can_id == can_id) {
            return &configured_nodes[i];
        }
    }

    return NULL;
}

static void mark_all_low_power_acks_pending(void)
{
    for (size_t i = 0; i < configured_node_count; i++) {
        configured_nodes[i].low_power_ack_seen = false;
    }
}

static void print_node_power_summary(void)
{
    ESP_LOGI(TAG, "Configured node power status:");
    for (size_t i = 0; i < configured_node_count; i++) {
        const node_config_entry_t *node = &configured_nodes[i];
        const char *state = node->active ? "active" : "low-power/offline";
        const char *stop_ack = node->low_power_ack_seen ? "stop-ack=yes" : "stop-ack=no";

        ESP_LOGI(TAG,
                 "  node_id=%u name=%s state=%s %s last_state_rx_us=%lld",
                 node->node_id,
                 node->name,
                 state,
                 stop_ack,
                 (long long)node->last_state_rx_us);

        for (size_t s = 0; s < node->sensor_count; s++) {
            const sensor_config_t *sensor = &node->sensors[s];
            ESP_LOGI(TAG,
                     "    sensor_id=%u name=%s can_id=0x%03" PRIX32 " units=%s",
                     sensor->sensor_id,
                     sensor->name,
                     sensor->can_id,
                     sensor->units);
        }
    }
}

static void handle_node_state_frame(const can_rx_word_t *msg)
{
    node_config_entry_t *node = find_configured_node_by_state_can_id(msg->id);
    uint8_t state = (uint8_t)(msg->data & 0xFFu);

    if (!node) {
        ESP_LOGW(TAG, "State ACK from unconfigured node frame 0x%03" PRIX32 " state=%u",
                 msg->id, state);
        return;
    }

    node->last_state_rx_us = esp_timer_get_time();

    if (state == NODE_STATE_ACTIVE) {
        node->active = true;
        node->low_power_ack_seen = false;
        ESP_LOGI(TAG, "Node %u (%s) confirmed ACTIVE", node->node_id, node->name);
    } else if (state == NODE_STATE_LOW_POWER) {
        node->active = false;
        node->low_power_ack_seen = true;
        ESP_LOGI(TAG, "Node %u (%s) confirmed LOW-POWER/OFF", node->node_id, node->name);
    } else {
        ESP_LOGW(TAG, "Node %u (%s) sent unknown state=%u",
                 node->node_id, node->name, state);
    }
}

static bool start_logging_session(void)
{
    if (log_state != LOG_STATE_IDLE) {
        ESP_LOGW(TAG, "Start ignored; logger is %s", log_state_name(log_state));
        return false;
    }

    create_unique_log_filename(current_log_path, sizeof(current_log_path));

    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        return false;
    }

    close_log_file_if_open();
    logFile = fopen(current_log_path, "wb");
    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Failed to open log file: %s", current_log_path);
        return false;
    }
    xSemaphoreGive(log_file_mutex);

    xQueueReset(twai_rx_sample_queue);
    writeCounter = 0;
    log_state = LOG_STATE_RUNNING;

    for (size_t i = 0; i < configured_node_count; i++) {
        configured_nodes[i].low_power_ack_seen = false;
    }

    ESP_LOGI(TAG, "Logging started: %s", current_log_path);
    (void)send_control_command(ID_START_CMD);
    return true;
}

static bool stop_logging_session(void)
{
    if (log_state != LOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Stop ignored; logger is %s", log_state_name(log_state));
        return false;
    }

    log_state = LOG_STATE_STOPPING;
    ESP_LOGI(TAG, "Logging stop requested; draining queued samples...");
    mark_all_low_power_acks_pending();
    (void)send_control_command(ID_STOP_CMD);
    vTaskDelay(pdMS_TO_TICKS(NODE_LOW_POWER_ACK_WAIT_MS));
    print_node_power_summary();
    return true;
}

static void print_sd_card_status(void)
{
    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) == pdTRUE) {
        if (logFile) {
            fflush(logFile);
        }
        xSemaphoreGive(log_file_mutex);
    }

    ESP_LOGI(TAG, "SD files:");

    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open /sdcard");
    } else {
        struct dirent *entry;
        int file_count = 0;

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char path[LOG_PATH_MAX_LEN + 32];
            int needed = snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
            if (needed < 0 || needed >= (int)sizeof(path)) {
                ESP_LOGW(TAG, "Skipping long filename: %s", entry->d_name);
                continue;
            }

            struct stat st;
            if (stat(path, &st) == 0) {
                ESP_LOGI(TAG, "  %s | %lld bytes", entry->d_name, (long long)st.st_size);
                file_count++;
            } else {
                ESP_LOGW(TAG, "  %s | size unavailable", entry->d_name);
            }
        }

        closedir(dir);

        if (file_count == 0) {
            ESP_LOGI(TAG, "  (empty)");
        }
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT fr = f_getfree("/sdcard", &free_clusters, &fs);
    if (fr == FR_OK && fs != NULL) {
        uint64_t total_clusters = (uint64_t)(fs->n_fatent - 2);
        uint64_t sectors_per_cluster = (uint64_t)fs->csize;
        uint64_t bytes_per_sector = FF_MIN_SS;
        uint64_t total_bytes = total_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t free_bytes = (uint64_t)free_clusters * sectors_per_cluster * bytes_per_sector;
        uint64_t used_bytes = total_bytes - free_bytes;
        uint64_t free_pct_x10 = total_bytes > 0
                              ? (free_bytes * 1000ULL) / total_bytes
                              : 0;
        uint64_t used_pct_x10 = total_bytes > 0
                              ? (used_bytes * 1000ULL) / total_bytes
                              : 0;

        double used_gb = (double)used_bytes / 1000000000.0;
        double free_gb = (double)free_bytes / 1000000000.0;
        double total_gb = (double)total_bytes / 1000000000.0;

        ESP_LOGI(TAG,
                 "SD space: used=%.2f GB (%llu.%01llu%%) free=%.2f GB (%llu.%01llu%%) total=%.2f GB",
                 used_gb,
                 (unsigned long long)(used_pct_x10 / 10),
                 (unsigned long long)(used_pct_x10 % 10),
                 free_gb,
                 (unsigned long long)(free_pct_x10 / 10),
                 (unsigned long long)(free_pct_x10 % 10),
                 total_gb);
    } else {
        ESP_LOGE(TAG, "Failed to read SD free/used space: FatFS result=%d", (int)fr);
    }
}

static void rx_dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        can_rx_word_t msg;
        if (xQueueReceive(twai_rx_queue, &msg, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        
        // Add to heartbeat Queue
        if (msg.id == ID_NODE1_HEARTBEAT) {
            if (xQueueSend(twai_rx_heartbeat_queue, &msg, 1) != pdPASS) {
                ESP_LOGW(TAG, "Heartbeat queue FULL - item dropped");
            }
        }

        if (msg.id >= ID_NODE_STATE_BASE &&
            msg.id < (ID_NODE_STATE_BASE + 0x40)) {
            handle_node_state_frame(&msg);
        }

        if (log_state == LOG_STATE_RUNNING) {
            if (xQueueSend(twai_rx_sample_queue, &msg, 1) != pdPASS) {
                ESP_LOGW(TAG, "Sample queue FULL - item dropped");
            }
        }

    }
}

/* --------------------- Tasks ------------------ */
static void write_log_block(const int64_t (*buf)[INFO_PER_SAMPLE], size_t sample_count)
{
    const size_t bytes = sample_count * INFO_PER_SAMPLE * sizeof(int64_t);

    if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock log file mutex");
        return;
    }

    if (!logFile) {
        xSemaphoreGive(log_file_mutex);
        ESP_LOGE(TAG, "Log file is not open");
        log_state = LOG_STATE_STOPPING;
        return;
    }

    size_t written = fwrite(buf, 1, bytes, logFile);
    fflush(logFile);
    xSemaphoreGive(log_file_mutex);

    if (written != bytes) {
        ESP_LOGE(TAG, "Short write! wrote=%u expected=%u",
                 (unsigned)written, (unsigned)bytes);
        return;
    }

    writeCounter++;
    ESP_LOGI(TAG, "Wrote block %d (%u bytes, %u samples)",
             writeCounter, (unsigned)bytes, (unsigned)sample_count);
}

static void sd_writer_task(void *arg)
{
    (void)arg;
    size_t sample_index = 0;
    int64_t log_buffer[SAMPLES_PER_BLOCK][INFO_PER_SAMPLE];

    while (1) { 
        can_rx_word_t msg;
        BaseType_t got_sample = xQueueReceive(twai_rx_sample_queue, &msg, pdMS_TO_TICKS(100));

        if (got_sample == pdTRUE && log_state != LOG_STATE_IDLE) {
            uint32_t value32 = (uint32_t)(msg.data & 0xFFFFFFFFu);
            uint32_t ts32    = (uint32_t)((msg.data >> 32) & 0xFFFFFFFFu);

            ESP_LOGI(TAG,
                     "RX | ID: 0x%03" PRIX32 " | Timestamp: %" PRIu32 " | Value: %" PRIu32,
                     msg.id,
                     ts32,
                     value32);

            log_buffer[sample_index][0] = (int64_t)msg.id;
            log_buffer[sample_index][1] = (int64_t)msg.data;
            sample_index++;

            if (sample_index >= SAMPLES_PER_BLOCK) {
                write_log_block(log_buffer, sample_index);
                sample_index = 0;
            }
        }

        if (log_state == LOG_STATE_STOPPING && uxQueueMessagesWaiting(twai_rx_sample_queue) == 0) {
            if (sample_index > 0) {
                ESP_LOGI(TAG, "Flushing partial block (%u samples)...", (unsigned)sample_index);
                write_log_block(log_buffer, sample_index);
                sample_index = 0;
            }

            if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) == pdTRUE) {
                close_log_file_if_open();
                xSemaphoreGive(log_file_mutex);
            }

            log_state = LOG_STATE_IDLE;
            ESP_LOGI(TAG, "Logging stopped.");
        }
    }
}

static void save_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(LOG_SAVE_INTERVAL_MS));

        if (log_state != LOG_STATE_RUNNING) {
            continue;
        }

        if (xSemaphoreTake(log_file_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to lock log file mutex");
            continue;
        }

        if (logFile) {
            ESP_LOGI(TAG, "File saved");
            close_log_file_if_open();
            if (!reopen_log_file_append(current_log_path)) {
                log_state = LOG_STATE_STOPPING;
            }
        }

        xSemaphoreGive(log_file_mutex);
    }
}

static void serial_command_task(void *arg)
{
    (void)arg;
    char line[32];

    ESP_LOGI(TAG, "Serial commands ready: start, stop, status");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char *cmd = line;
        while (isspace((unsigned char)*cmd)) {
            cmd++;
        }

        char *end = cmd + strlen(cmd);
        while (end > cmd && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }

        for (char *p = cmd; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }

        if (strcmp(cmd, "start") == 0) {
            (void)start_logging_session();
        } else if (strcmp(cmd, "stop") == 0) {
            (void)stop_logging_session();
        } else if (strcmp(cmd, "status") == 0) {
            ESP_LOGI(TAG, "Logger status: %s, file=%s, blocks=%d",
                     log_state_name(log_state),
                     current_log_path[0] ? current_log_path : "(none)",
                     writeCounter);
            print_node_power_summary();
            print_sd_card_status();
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            ESP_LOGI(TAG, "Commands: start, stop, status");
        } else if (cmd[0] != '\0') {
            ESP_LOGW(TAG, "Unknown command: %s", cmd);
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
        ESP_LOGI(TAG,
                         "TX Data | ts=%" PRIu64,
                         t_main_us);
        vTaskDelay(pdMS_TO_TICKS(TIME_BEACON_PERIOD_MS));
    }
 
    vTaskDelete(NULL);
}

/* Tasks to add:
Heartbeat
Error Handling (reset CAN bus)
dispatcher (duplicate info into multi queues? Wifi queue, SD queue, Motor Queue)
*/



/* --------------------- app_main ------------------ */

void app_main(void)
{
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


    // ---------------- QUEU/SEMAPHORE INIT ------------------
    twai_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI RX queue");
        return;
    }

    twai_rx_heartbeat_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_heartbeat_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI heartbeat queue");
        return;
    }

    twai_rx_sample_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(can_rx_word_t));
    if (!twai_rx_sample_queue) {
        ESP_LOGE(TAG, "Failed to create TWAI sample queue");
        return;
    }

    log_file_mutex = xSemaphoreCreateMutex();
    if (!log_file_mutex) {
        ESP_LOGE(TAG, "Failed to create log_file_mutex");
        return;
    }


    // -------------------- START CAN -----------------------------
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled");

    xTaskCreate(sd_writer_task, "sd_writer", 8192, NULL, 10, &sd_task_handle);
    xTaskCreate(save_task,      "save_task", 4096, NULL, 10, &save_task_handle);
    xTaskCreate(serial_command_task, "serial_cmd", 4096, NULL, 9, &serial_task_handle);
    xTaskCreate(rx_dispatch_task, "dispatch",    4096, NULL,        8,  &dispatch_task_handle);
    //xTaskCreatePinnedToCore(time_beacon_task, "time_beacon", 4096, NULL, 7, NULL, tskNO_AFFINITY);

    ESP_LOGI(TAG, "Data logger ready. Type 'start' to begin logging.");
}
