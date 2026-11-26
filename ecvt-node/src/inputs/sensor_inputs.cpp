#include "app/modules_internal.h"



uint32_t decode_sensor_value_u32(const can_rx_item_t *item)
{
    uint32_t value = 0;
    const uint8_t copy_len = (item->dlc < sizeof(value)) ? item->dlc : sizeof(value);

    memcpy(&value, item->data, copy_len);
    return value;
}

float decode_sensor_value_f32(const can_rx_item_t *item)
{
    uint32_t value_bits = decode_sensor_value_u32(item);
    float value = 0.0f;

    memcpy(&value, &value_bits, sizeof(value));
    return value;
}

uint32_t can_sensor_id_value(can_sensor_id_t sensor_id)
{
    return can_sensor_map_get_id(sensor_id, NODE_ID);
}

bool enqueue_replay_sensor(can_sensor_id_t sensor_id, uint32_t timestamp, uint32_t value)
{
    can_rx_item_t item = {0};
    item.received_us = esp_timer_get_time();
    const uint64_t packed_data = ((uint64_t)timestamp << 32) | (uint64_t)value;

    item.id = can_sensor_id_value(sensor_id);
    item.dlc = 8;
    memcpy(item.data, &packed_data, sizeof(packed_data));

    if (xQueueSend(twai_rx_raw_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "RAW queue full — dropping %s sample", can_sensor_map_name(sensor_id));
        return false;
    }

    return true;
}

bool enqueue_replay_sensor_f32(can_sensor_id_t sensor_id, uint32_t timestamp, float value)
{
    uint32_t value_bits = 0;

    memcpy(&value_bits, &value, sizeof(value_bits));
    return enqueue_replay_sensor(sensor_id, timestamp, value_bits);
}
