#pragma once
#include "app/runtime_internal.h"

uint32_t decode_sensor_value_u32(const can_rx_item_t *item);

float decode_sensor_value_f32(const can_rx_item_t *item);

uint32_t can_sensor_id_value(can_sensor_id_t sensor_id);

bool enqueue_replay_sensor(can_sensor_id_t sensor_id, uint32_t timestamp, uint32_t value);

bool enqueue_replay_sensor_f32(can_sensor_id_t sensor_id, uint32_t timestamp, float value);
