#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAN_BITRATE 1000000

#define CAN_ID_STOP                 0x0A0
#define CAN_ID_START                0x0A1
#define CAN_ID_MASTER_TIME          0x0A2

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
#define CAN_ID_CVT_TEMP             0x0BD

/* Master-only log records, never sent on the bus. */
#define CAN_ID_GPS_SPEED            0x700
#define CAN_ID_GPS_LATITUDE         0x701
#define CAN_ID_GPS_LONGITUDE        0x702

#define CAN_MASTER_TIME_PERIOD_MS      100
#define CAN_MASTER_TIME_RECORDING_FLAG (UINT64_C(1) << 63)
#define CAN_MASTER_TIME_VALUE_MASK     (CAN_MASTER_TIME_RECORDING_FLAG - 1)

/* Node N sends state, reason and reset_reason (one byte each) on CAN_ID_NODE_STATE_BASE + N. */
#define CAN_ID_NODE_STATE_BASE      0x0C0
#define CAN_NODE_STATE_LENGTH       3
/* Bit N set: node N may be on the bus. Read right to left: nodes 6..1 on, bit 0 unused. */
#define CAN_NODE_ID_MASK            0b1111110U
#define CAN_NODE_SLOT_COUNT         7

_Static_assert(CAN_NODE_ID_MASK < (1U << CAN_NODE_SLOT_COUNT), "every node ID must be below CAN_NODE_SLOT_COUNT");

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

/* Node that sends a sample frame (int32 value, uint32 timestamp_ms); 0 for any other ID. */
static inline uint8_t protocol_sensor_node_id(uint32_t id)
{
    switch (id) {
        case CAN_ID_FRONT_BRAKE:
        case CAN_ID_REAR_BRAKE:     return 1;
        case CAN_ID_CVT_TEMP:       return 2;
        case CAN_ID_MPU_ACCEL_X:
        case CAN_ID_MPU_ACCEL_Y:
        case CAN_ID_MPU_ACCEL_Z:
        case CAN_ID_MPU_GYRO_X:
        case CAN_ID_MPU_GYRO_Y:
        case CAN_ID_MPU_GYRO_Z:     return 3;
        case CAN_ID_BEARING_ENCODER: return 4;
        case CAN_ID_ENGINE_RPM:
        case CAN_ID_ENGINE_SPARK:   return 5;
        case CAN_ID_GENERIC_ADC:    return 6;
        default:                    return 0;
    }
}

static inline bool protocol_is_sensor_id(uint32_t id)
{
    return protocol_sensor_node_id(id) != 0;
}
