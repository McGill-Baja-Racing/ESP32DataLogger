#include "can/can_master.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"

#define CAN_TX_GPIO             20
#define CAN_RX_GPIO             21
#ifndef MASTER_CAN_BITRATE
#define MASTER_CAN_BITRATE      1000000
#endif
#define CAN_RX_QUEUE_LENGTH     256
#define CAN_TX_QUEUE_DEPTH      5
#define CAN_RECOVERY_POLL_MS    250
#define CAN_COMMAND_REPEAT_COUNT 5
#define CAN_COMMAND_REPEAT_MS    20

static const char *TAG = "MasterCAN";
static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_mutex;
static twai_node_handle_t can_handle;
static can_message_handler_t application_handler;
static volatile uint32_t rx_drops;

static bool receive_callback(twai_node_handle_t node,
                             const twai_rx_done_event_data_t *event,
                             void *context)
{
    (void)event;
    (void)context;
    BaseType_t task_woken = pdFALSE;
    uint8_t bytes[8] = {0};
    twai_frame_t frame = {.buffer = bytes, .buffer_len = sizeof(bytes)};
    if (twai_node_receive_from_isr(node, &frame) == ESP_OK) {
        can_message_t message = {
            .id = frame.header.id,
            .dlc = frame.header.dlc > 8 ? 8 : frame.header.dlc,
        };
        for (uint8_t i = 0; i < message.dlc; i++) {
            message.data |= (uint64_t)bytes[i] << (8 * i);
        }
        if (xQueueSendFromISR(rx_queue, &message, &task_woken) != pdPASS) {
            rx_drops++;
        }
    }
    return task_woken == pdTRUE;
}

esp_err_t can_master_send(uint32_t id, const uint8_t *payload, uint8_t length)
{
    if (length > 8 || (length > 0 && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tx_mutex || xSemaphoreTake(tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    twai_frame_t frame = {
        .header.id = id,
        .header.ide = false,
        .buffer = (uint8_t *)payload,
        .buffer_len = length,
    };
    esp_err_t error = twai_node_transmit(can_handle, &frame, 100);
    if (error == ESP_OK) {
        /*
         * The TWAI driver queues pointers to the frame and payload instead of
         * copying them. Wait before these stack objects leave scope.
         */
        error = twai_node_transmit_wait_all_done(can_handle, -1);
    }
    xSemaphoreGive(tx_mutex);
    return error;
}

static esp_err_t send_command_burst(uint32_t id)
{
    esp_err_t first_error = ESP_OK;
    for (unsigned int attempt = 0; attempt < CAN_COMMAND_REPEAT_COUNT; attempt++) {
        esp_err_t error = can_master_send(id, NULL, 0);
        if (error != ESP_OK && first_error == ESP_OK) {
            first_error = error;
        }
        if (attempt + 1 < CAN_COMMAND_REPEAT_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(CAN_COMMAND_REPEAT_MS));
        }
    }
    return first_error;
}

esp_err_t can_master_start_nodes(void)
{
    return send_command_burst(CAN_ID_START);
}

esp_err_t can_master_stop_nodes(void)
{
    return send_command_burst(CAN_ID_STOP);
}

uint32_t can_master_rx_drop_count(void)
{
    return rx_drops;
}

static void dispatch_task(void *argument)
{
    (void)argument;
    can_message_t message;
    while (true) {
        if (xQueueReceive(rx_queue, &message, portMAX_DELAY) == pdTRUE &&
            application_handler) {
            application_handler(&message);
        }
    }
}

static const char *state_name(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE: return "active";
    case TWAI_ERROR_WARNING: return "warning";
    case TWAI_ERROR_PASSIVE: return "passive";
    case TWAI_ERROR_BUS_OFF: return "bus-off";
    default: return "unknown";
    }
}

static void recovery_task(void *argument)
{
    (void)argument;
    bool recovering = false;
    twai_error_state_t previous = TWAI_ERROR_ACTIVE;
    while (true) {
        twai_node_status_t status = {0};
        twai_node_record_t record = {0};
        esp_err_t error = twai_node_get_info(can_handle, &status, &record);
        if (error == ESP_OK) {
            if (status.state != previous) {
                ESP_LOGW(TAG, "State %s -> %s; tx=%u rx=%u errors=%" PRIu32,
                         state_name(previous), state_name(status.state),
                         status.tx_error_count, status.rx_error_count,
                         record.bus_err_num);
                previous = status.state;
            }
            if (status.state == TWAI_ERROR_BUS_OFF && !recovering) {
                recovering = twai_node_recover(can_handle) == ESP_OK;
            } else if (recovering && status.state == TWAI_ERROR_ACTIVE) {
                recovering = false;
                ESP_LOGI(TAG, "CAN recovered and is error-active");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_POLL_MS));
    }
}

esp_err_t can_master_init(can_message_handler_t message_handler)
{
    if (!message_handler) {
        return ESP_ERR_INVALID_ARG;
    }
    application_handler = message_handler;
    rx_queue = xQueueCreate(CAN_RX_QUEUE_LENGTH, sizeof(can_message_t));
    tx_mutex = xSemaphoreCreateMutex();
    if (!rx_queue || !tx_mutex) {
        return ESP_ERR_NO_MEM;
    }
    twai_onchip_node_config_t config = {
        .io_cfg = {.tx = CAN_TX_GPIO, .rx = CAN_RX_GPIO},
        .bit_timing = {.bitrate = MASTER_CAN_BITRATE},
        .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
    };
    esp_err_t error = twai_new_node_onchip(&config, &can_handle);
    if (error != ESP_OK) return error;
    twai_event_callbacks_t callbacks = {.on_rx_done = receive_callback};
    error = twai_node_register_event_callbacks(can_handle, &callbacks, NULL);
    if (error != ESP_OK) return error;
    error = twai_node_enable(can_handle);
    if (error != ESP_OK) return error;
    if (xTaskCreate(recovery_task, "can_recovery", 3072, NULL, 9, NULL) != pdPASS ||
        xTaskCreate(dispatch_task, "can_dispatch", 3072, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
