#ifndef CAN_SENSOR_MAP_H
#define CAN_SENSOR_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
#define CAN_SENSOR_ENTRY(sensor_id, name, description, base_id, message_type) sensor_id,
#include "can_sensor_map_entries.inc"
#undef CAN_SENSOR_ENTRY
    CAN_SENSOR_COUNT
} can_sensor_id_t;

typedef enum {
    CAN_SENSOR_MAP_TYPE_MOTOR_OBJECT = 0,
    CAN_SENSOR_MAP_TYPE_SENSOR_DATA,
} can_sensor_map_type_t;

typedef struct {
    can_sensor_id_t sensor_id;
    const char *name;
    const char *description;
    uint32_t base_id;
    can_sensor_map_type_t message_type;
} can_sensor_map_entry_t;

extern const can_sensor_map_entry_t g_can_sensor_map[CAN_SENSOR_COUNT];

uint32_t can_sensor_map_get_id(can_sensor_id_t sensor_id, uint8_t node_id);
const can_sensor_map_entry_t *can_sensor_map_find_by_id(uint32_t can_id, uint8_t node_id);
const char *can_sensor_map_name(can_sensor_id_t sensor_id);

#ifdef __cplusplus
}
#endif

#endif
