#include "can_node.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * CAN/TWAI transport
 * ------------------
 * The ISR copies received frames into rx_queue. dispatch_task decodes only the
 * application control protocol and invokes callbacks supplied by main.c.
 * recovery_task owns bus-off monitoring and recovery.
 */

#ifndef NODE_CAN_TX_GPIO
#define NODE_CAN_TX_GPIO 21
#endif
#ifndef NODE_CAN_RX_GPIO
#define NODE_CAN_RX_GPIO 20
#endif
#ifndef NODE_CAN_BITRATE
#define NODE_CAN_BITRATE 1000000
#endif

#define CAN_TX_QUEUE_DEPTH      5
#define CAN_RX_QUEUE_LENGTH     16
#define CAN_TX_TIMEOUT_MS       50
#define CAN_RECOVERY_POLL_MS    250
#define CAN_ERROR_LOG_MS        1000

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint64_t data;
} can_message_t;

static const char *TAG = "CAN";
static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_mutex;
static twai_node_handle_t can_handle;
static can_node_callbacks_t app_callbacks;
static node_state_t current_state = NODE_STATE_IDLE;
static uint8_t reset_reason;

static bool receive_callback(twai_node_handle_t node,
                             const twai_rx_done_event_data_t *event,
                             void *context)
{
    /* Keep ISR work bounded: copy the frame and defer all handling to a task. */
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
        (void)xQueueSendFromISR(rx_queue, &message, &task_woken);
    }
    return task_woken == pdTRUE;
}

static bool targets_this_node(const can_message_t *message)
{
    if (message->dlc == 0) {
        return true;
    }
    uint8_t target = (uint8_t)message->data;
    return target == NODE_ID || target == 0xFF;
}

esp_err_t can_node_send(uint32_t can_id, const uint8_t *payload, uint8_t length)
{
    if (length > 8 || (length > 0 && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tx_mutex ||
        xSemaphoreTake(tx_mutex, pdMS_TO_TICKS(CAN_TX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    twai_frame_t frame = {
        .header.id = can_id,
        .header.ide = false,
        .buffer = (uint8_t *)payload,
        .buffer_len = length,
    };
    esp_err_t error = twai_node_transmit(can_handle, &frame, CAN_TX_TIMEOUT_MS);
    if (error == ESP_OK) {
        /* The TWAI queue retains these pointers until transmission finishes. */
        error = twai_node_transmit_wait_all_done(can_handle, -1);
    }
    xSemaphoreGive(tx_mutex);
    return error;
}

void can_node_report_state(node_state_t state, node_state_reason_t reason)
{
    current_state = state;
    uint8_t payload[3] = {
        state,
        reason,
        reason == NODE_STATE_REASON_BOOT ? reset_reason : 0,
    };
    esp_err_t error = can_node_send(CAN_ID_NODE_STATE, payload, sizeof(payload));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "State report failed: %s", esp_err_to_name(error));
    }
}

static void dispatch_task(void *argument)
{
    /* This is the single receive-side protocol dispatch point. */
    (void)argument;
    can_message_t message;
    while (true) {
        xQueueReceive(rx_queue, &message, portMAX_DELAY);
        if (message.id == CAN_ID_MASTER_TIME && message.dlc == 8) {
            if (app_callbacks.on_time_beacon) {
                app_callbacks.on_time_beacon(message.data);
            }
        } else if (message.id == CAN_ID_START && targets_this_node(&message)) {
            if (current_state != NODE_STATE_ACTIVE) {
                ESP_LOGI(TAG, "Received START; sampler -> active");
                if (app_callbacks.on_start) {
                    app_callbacks.on_start();
                }
            }
            /* Acknowledge every copy in the command burst. Duplicate STARTs
             * do not restart the sampler, but replace a lost acknowledgement. */
            can_node_report_state(NODE_STATE_ACTIVE, NODE_STATE_REASON_START);
        } else if (message.id == CAN_ID_STOP && targets_this_node(&message)) {
            if (current_state != NODE_STATE_IDLE) {
                ESP_LOGI(TAG, "Received STOP; sampler -> idle");
                if (app_callbacks.on_stop) {
                    app_callbacks.on_stop();
                }
            }
            /* As with START, repeat only the acknowledgement, not the action. */
            can_node_report_state(NODE_STATE_IDLE, NODE_STATE_REASON_STOP);
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
    /* Recovery preserves current_state; only unsent samples are discarded. */
    (void)argument;
    bool recovering = false;
    twai_error_state_t previous = TWAI_ERROR_ACTIVE;
    uint8_t previous_tx_errors = 0;
    uint8_t previous_rx_errors = 0;
    uint32_t previous_bus_errors = 0;
    TickType_t last_error_log = 0;
    while (true) {
        twai_node_status_t status = {0};
        twai_node_record_t record = {0};
        if (twai_node_get_info(can_handle, &status, &record) == ESP_OK) {
            bool state_changed = status.state != previous;
            if (state_changed) {
                ESP_LOGW(TAG, "State %s -> %s; tx=%u rx=%u errors=%" PRIu32,
                         state_name(previous), state_name(status.state),
                         status.tx_error_count, status.rx_error_count,
                         record.bus_err_num);
                previous = status.state;
            }
            TickType_t now = xTaskGetTickCount();
            bool errors_changed = status.tx_error_count != previous_tx_errors ||
                                  status.rx_error_count != previous_rx_errors ||
                                  record.bus_err_num != previous_bus_errors;
            if (errors_changed &&
                now - last_error_log >= pdMS_TO_TICKS(CAN_ERROR_LOG_MS)) {
                ESP_LOGW(TAG, "CAN errors while %s: tx=%u rx=%u total=%" PRIu32,
                         state_name(status.state), status.tx_error_count,
                         status.rx_error_count, record.bus_err_num);
                last_error_log = now;
            }
            previous_tx_errors = status.tx_error_count;
            previous_rx_errors = status.rx_error_count;
            previous_bus_errors = record.bus_err_num;
            /* A failed recovery can pass through PASSIVE and return to BUS_OFF.
             * Start a fresh recovery each time BUS_OFF is entered so a later
             * physical reconnection can restore communication automatically. */
            if (status.state == TWAI_ERROR_BUS_OFF &&
                (!recovering || state_changed)) {
                if (app_callbacks.on_bus_off) {
                    app_callbacks.on_bus_off();
                }
                esp_err_t error = twai_node_recover(can_handle);
                recovering = error == ESP_OK;
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "CAN recovery start failed: %s",
                             esp_err_to_name(error));
                }
            } else if (recovering && status.state == TWAI_ERROR_ACTIVE) {
                recovering = false;
                ESP_LOGI(TAG, "CAN recovered; sampler remains %s",
                         current_state == NODE_STATE_ACTIVE ? "active" : "idle");
                can_node_report_state(current_state, NODE_STATE_REASON_RECOVERY);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_POLL_MS));
    }
}

esp_err_t can_node_init(const can_node_callbacks_t *callbacks,
                        uint8_t boot_reset_reason)
{
    if (!callbacks) {
        return ESP_ERR_INVALID_ARG;
    }
    app_callbacks = *callbacks;
    reset_reason = boot_reset_reason;
    rx_queue = xQueueCreate(CAN_RX_QUEUE_LENGTH, sizeof(can_message_t));
    tx_mutex = xSemaphoreCreateMutex();
    if (!rx_queue || !tx_mutex) {
        return ESP_ERR_NO_MEM;
    }

    twai_onchip_node_config_t config = {
        .io_cfg = {.tx = NODE_CAN_TX_GPIO, .rx = NODE_CAN_RX_GPIO},
        .bit_timing = {.bitrate = NODE_CAN_BITRATE},
        .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
    };
    esp_err_t error = twai_new_node_onchip(&config, &can_handle);
    if (error != ESP_OK) {
        return error;
    }
    twai_event_callbacks_t driver_callbacks = {.on_rx_done = receive_callback};
    error = twai_node_register_event_callbacks(can_handle, &driver_callbacks, NULL);
    if (error != ESP_OK) {
        return error;
    }
    error = twai_node_enable(can_handle);
    if (error != ESP_OK) {
        return error;
    }
    if (xTaskCreate(dispatch_task, "can_dispatch", 3072, NULL, 8, NULL) != pdPASS ||
        xTaskCreate(recovery_task, "can_recovery", 3072, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
