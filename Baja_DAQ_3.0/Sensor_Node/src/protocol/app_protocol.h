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
#define CAN_ID_MPU_ACCEL_X          0x0B3
#define CAN_ID_MPU_ACCEL_Y          0x0B4
#define CAN_ID_MPU_ACCEL_Z          0x0B5
#define CAN_ID_MPU_GYRO_X           0x0B6
#define CAN_ID_MPU_GYRO_Y           0x0B7
#define CAN_ID_MPU_GYRO_Z           0x0B8
#define CAN_ID_BEARING_ENCODER      0x0B9
#define CAN_ID_GENERIC_ADC          0x0BA
#define CAN_ID_ENGINE_RPM           0x0BB
#define CAN_ID_ENGINE_SPARK         0x0BC

#define CAN_MASTER_TIME_RECORDING_FLAG (UINT64_C(1) << 63)
#define CAN_MASTER_TIME_VALUE_MASK     (CAN_MASTER_TIME_RECORDING_FLAG - 1)

typedef enum {
    NODE_STATE_IDLE = 0,
    NODE_STATE_ACTIVE = 1,
} node_state_t;

typedef enum {
    NODE_STATE_REASON_BOOT = 1,
    NODE_STATE_REASON_STOP = 2,
    NODE_STATE_REASON_START = 3,
    NODE_STATE_REASON_RECOVERY = 4,
    NODE_STATE_REASON_BEACON_SYNC = 5,
} node_state_reason_t;
