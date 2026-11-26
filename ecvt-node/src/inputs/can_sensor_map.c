#include "can_sensor_map.h"

const can_sensor_map_entry_t g_can_sensor_map[CAN_SENSOR_COUNT] = {
#define CAN_SENSOR_ENTRY(entry_sensor_id, entry_name, entry_description, entry_base_id, entry_message_type) \
    [entry_sensor_id] = { \
        .sensor_id = entry_sensor_id, \
        .name = entry_name, \
        .description = entry_description, \
        .base_id = entry_base_id, \
        .message_type = entry_message_type, \
    },
#include "can_sensor_map_entries.inc"
#undef CAN_SENSOR_ENTRY
};

uint32_t can_sensor_map_get_id(can_sensor_id_t sensor_id, uint8_t node_id)
{
    const can_sensor_map_entry_t *entry = &g_can_sensor_map[sensor_id];
    return (entry->message_type == CAN_SENSOR_MAP_TYPE_MOTOR_OBJECT) ? (entry->base_id + node_id) : entry->base_id;
}

const can_sensor_map_entry_t *can_sensor_map_find_by_id(uint32_t can_id, uint8_t node_id)
{
    for (int i = 0; i < CAN_SENSOR_COUNT; ++i) {
        const can_sensor_map_entry_t *entry = &g_can_sensor_map[i];
        if (can_sensor_map_get_id(entry->sensor_id, node_id) == can_id) {
            return entry;
        }
    }
    return 0;
}

const char *can_sensor_map_name(can_sensor_id_t sensor_id)
{
    return g_can_sensor_map[sensor_id].name;
}
