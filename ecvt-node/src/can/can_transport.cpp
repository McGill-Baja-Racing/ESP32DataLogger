#include "can_transport.h"
#include "servo_frame.h"
#include "servo/ECVT_motor.h"
#include "servo/servo_feedback.h"
#include "esp_twai_onchip.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <cstring>

namespace {
constexpr const char *TAG = "ECVT";
QueueHandle_t raw_queue, sdo_queue;
twai_node_handle_t node;

bool receive(twai_node_handle_t handle, const twai_rx_done_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    uint8_t data[8] = {};
    twai_frame_t frame{};
    frame.buffer = data;
    frame.buffer_len = sizeof(data);
    if (twai_node_receive_from_isr(handle, &frame) == ESP_OK) {
        ecvt_motor_can_rx_item_t item{};
        item.received_us = esp_timer_get_time();
        item.id = frame.header.id;
        item.dlc = frame.header.dlc > 8 ? 8 : frame.header.dlc;
        std::memcpy(item.data, data, item.dlc);
        xQueueSendFromISR(raw_queue, &item, &woken);
    }
    return woken == pdTRUE;
}

void dispatch(void *)
{
    while (true) {
        ecvt_motor_can_rx_item_t item{};
        if (xQueueReceive(raw_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        switch (servo_frame_kind(item.id, item.dlc)) {
        case ServoFrameKind::Sdo:
            if (xQueueSend(sdo_queue, &item, 1) != pdPASS)
                ESP_LOGW(TAG, "Servo SDO queue full; reply dropped");
            break;
        case ServoFrameKind::Status:
            servo_feedback_record(1, servo_frame_u16(item.data), item.received_us);
            break;
        case ServoFrameKind::Position: {
            const int32_t value = static_cast<int32_t>(servo_frame_u32(item.data));
            (void)ecvt_motor_set_actual_position_feedback(float(value) / 10000.0f);
            servo_feedback_record(2, value, item.received_us);
            break;
        }
        case ServoFrameKind::Velocity:
            servo_feedback_record(3, static_cast<int32_t>(servo_frame_u32(item.data)), item.received_us);
            break;
        case ServoFrameKind::Current:
            servo_feedback_record(4, static_cast<int16_t>(servo_frame_u16(item.data)), item.received_us);
            break;
        case ServoFrameKind::Ignore: break;
        }
    }
}
} // namespace

esp_err_t servo_can_start()
{
    if (raw_queue || sdo_queue || node) return ESP_ERR_INVALID_STATE;
    raw_queue = xQueueCreate(256, sizeof(ecvt_motor_can_rx_item_t));
    sdo_queue = xQueueCreate(256, sizeof(ecvt_motor_can_rx_item_t));
    if (!raw_queue || !sdo_queue) return ESP_ERR_NO_MEM;
    twai_onchip_node_config_t config{};
    config.io_cfg.tx = GPIO_NUM_5;
    config.io_cfg.rx = GPIO_NUM_4;
    config.bit_timing.bitrate = 500000;
    config.tx_queue_depth = 8;
    esp_err_t error = twai_new_node_onchip(&config, &node);
    if (error != ESP_OK) return error;
    twai_event_callbacks_t callbacks{};
    callbacks.on_rx_done = receive;
    error = twai_node_register_event_callbacks(node, &callbacks, nullptr);
    if (error != ESP_OK) return error;
    error = twai_node_enable(node);
    if (error != ESP_OK) return error;
    error = ecvt_motor_init(node, sdo_queue, 1, TAG);
    if (error != ESP_OK) return error;
    if (xTaskCreate(dispatch, "servo_rx", 4096, nullptr, 7, nullptr) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Standalone servo CAN ready: 500 kbit/s, node 1, TX5/RX4");
    return ESP_OK;
}
