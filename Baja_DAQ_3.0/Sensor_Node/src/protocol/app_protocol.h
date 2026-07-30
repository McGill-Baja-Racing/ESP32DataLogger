#pragma once

/*
 * Sensor-node side of the shared master/node CAN protocol. Keep matching
 * constants in the Master_Node firmware synchronized when this file changes.
 */

#ifndef NODE_ID
#define NODE_ID 4
#endif

#define CAN_ID_STOP                 0x0A0
#define CAN_ID_START                0x0A1
#define CAN_ID_MASTER_TIME          0x0A2
#define CAN_ID_NODE_STATE           (0x0C0 + NODE_ID)

#define CAN_ID_FRONT_BRAKE          0x0B1
#define CAN_ID_REAR_BRAKE           0x0B2
#define CAN_ID_BEARING_ENCODER      0x0B9
#define CAN_ID_GENERIC_ADC          0x0BA
#define CAN_ID_ENGINE_RPM           0x0BB

typedef enum {
    NODE_STATE_IDLE = 0,
    NODE_STATE_ACTIVE = 1,
} node_state_t;

typedef enum {
    NODE_STATE_REASON_BOOT = 1,
    NODE_STATE_REASON_STOP = 2,
    NODE_STATE_REASON_START = 3,
    NODE_STATE_REASON_RECOVERY = 4,
} node_state_reason_t;
