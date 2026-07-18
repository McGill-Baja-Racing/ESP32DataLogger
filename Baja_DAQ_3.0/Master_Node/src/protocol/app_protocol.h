#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep these values identical to Sensor_Node/src/protocol/app_protocol.h. */
#define CAN_ID_STOP                 0x0A0
#define CAN_ID_START                0x0A1
#define CAN_ID_MASTER_TIME          0x0A2
#define CAN_ID_NODE_STATE_BASE      0x0C0

#define CAN_ID_FRONT_BRAKE          0x0B1
#define CAN_ID_REAR_BRAKE           0x0B2
#define CAN_ID_BEARING_ENCODER      0x0B9
#define CAN_ID_GENERIC_ADC          0x0BA
#define CAN_ID_ENGINE_RPM           0x0BB

typedef enum {
    PROTOCOL_NODE_IDLE = 0,
    PROTOCOL_NODE_ACTIVE = 1,
} protocol_node_state_t;

static inline bool protocol_is_sensor_id(uint32_t id)
{
    return id == CAN_ID_FRONT_BRAKE ||
           id == CAN_ID_REAR_BRAKE ||
           id == CAN_ID_BEARING_ENCODER ||
           id == CAN_ID_GENERIC_ADC ||
           id == CAN_ID_ENGINE_RPM;
}
