#include "app/modules_internal.h"
#include "servo/servo_feedback.h"

QueueHandle_t twai_rx_raw_queue;
QueueHandle_t twai_rx_sample_queue;
QueueHandle_t twai_rx_servo_SDO_queue;
twai_node_handle_t node_hdl = NULL;
twai_onchip_node_config_t node_cfg{};
twai_event_callbacks_t cbs{};
volatile uint32_t s_latest_throttle = 0;
volatile float s_latest_engine_rpm = 0.0f;
volatile float s_latest_wheel_rpm = 0.0f;
volatile bool s_latest_brake_state = false;
volatile uint16_t s_latest_tpdo1_statusword = 0;
volatile int64_t s_latest_tpdo1_rx_us = 0;

bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_woken = pdFALSE;

    uint8_t raw[8] = {0};
    twai_frame_t fr{};
    fr.buffer = raw;
    fr.buffer_len = sizeof(raw);


    if (twai_node_receive_from_isr(node, &fr) == ESP_OK) {
        can_rx_item_t item = {0};
        item.received_us = esp_timer_get_time();
        item.id  = fr.header.id;
        item.dlc = fr.header.dlc > 8 ? 8 : fr.header.dlc;
        memcpy(item.data, raw, item.dlc);
        xQueueSendFromISR(twai_rx_raw_queue, &item, &hp_woken);
    }

    return hp_woken == pdTRUE;
}

void rx_dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        can_rx_item_t item;
        if (xQueueReceive(twai_rx_raw_queue, &item, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        const can_sensor_map_entry_t *map_entry = can_sensor_map_find_by_id(item.id, NODE_ID);
        if (!map_entry) {
            continue;
        }

        if (map_entry->sensor_id == CAN_SENSOR_SERVO_SDO_RX) {
            if (xQueueSend(twai_rx_servo_SDO_queue, &item, 1) != pdPASS) {
                ESP_LOGW(TAG, "Servo Queue FULL — item dropped");
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO1) {
            if (item.dlc >= 2) {
                s_latest_tpdo1_statusword = (uint16_t)(item.data[0] | (item.data[1] << 8));
                s_latest_tpdo1_rx_us = item.received_us;
                servo_feedback_record(1, s_latest_tpdo1_statusword, s_latest_tpdo1_rx_us);
            }
        }
        else if (map_entry->message_type == CAN_SENSOR_MAP_TYPE_SENSOR_DATA) {
            const uint32_t sensor_value = decode_sensor_value_u32(&item);

            switch (map_entry->sensor_id) {
            case CAN_SENSOR_ENGINE_RPM:
                s_latest_engine_rpm = decode_sensor_value_f32(&item);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_ENGINE_RPM);
                }
                break;
            case CAN_SENSOR_BRAKE_STATE:
                s_latest_brake_state = (sensor_value != 0U);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_BRAKE_STATE);
                }
                break;
            case CAN_SENSOR_THROTTLE:
                s_latest_throttle = sensor_value;
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_THROTTLE);
                }
                break;
            case CAN_SENSOR_WHEEL_RPM:
                s_latest_wheel_rpm = decode_sensor_value_f32(&item);
                if (!stop_logging) {
                    queue_sensor_snapshot_log(LOG_RECORD_TYPE_SENSOR_WHEEL_RPM);
                }
                break;
            default:
                break;
            }

            if (xQueueSend(twai_rx_sample_queue, &item, 1) != pdPASS) {
                ESP_LOGW(TAG, "Sample Queue FULL — item dropped");
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO2) {
            if (item.dlc >= 4) {
                const int32_t position_counts = (int32_t)decode_sensor_value_u32(&item);
                (void)ecvt_motor_set_actual_position_feedback((float)position_counts / 10000.0f);
                servo_feedback_record(2, position_counts, item.received_us);
            }
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO2,
                                         item.dlc >= 4 ? (int32_t)decode_sensor_value_u32(&item) : 0);
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO3) {
            if (item.dlc >= 4) servo_feedback_record(3, (int32_t)decode_sensor_value_u32(&item), item.received_us);
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO3,
                                         item.dlc >= 4 ? (int32_t)decode_sensor_value_u32(&item) : 0);
            }
        }
        else if (map_entry->sensor_id == CAN_SENSOR_SERVO_TPDO4) {
            if (item.dlc >= 2) servo_feedback_record(4, static_cast<int16_t>(decode_sensor_value_u32(&item)), item.received_us);
            if (!stop_logging) {
                queue_servo_feedback_log(LOG_RECORD_TYPE_SERVO_TPDO4,
                                         item.dlc >= 2 ? (int32_t)(int16_t)decode_sensor_value_u32(&item) : 0);
            }
        }
    }
}
