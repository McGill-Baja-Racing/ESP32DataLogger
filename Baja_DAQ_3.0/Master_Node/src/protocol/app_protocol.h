#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep these values identical to Sensor_Node/src/protocol/app_protocol.h. */
#define CAN_ID_STOP                 0x0A0
#define CAN_ID_START                0x0A1
#define CAN_ID_MASTER_TIME          0x0A2
#define CAN_ID_NODE_STATE_BASE      0x0C0
#define CAN_ID_DIAGNOSTIC_BASE      0x0D0
#define DIAG_FLAG_ACTIVE            0x01
#define DIAG_FLAG_DATA_DEGRADED     0x08
#define DIAG_FLAG_TIME_VALID        0x10
#define DIAG_SEVERITY_SHIFT         1
#define DIAG_SEVERITY_MASK          0x06

#define DIAG_CAN_TX_FAILED          0x0201
#define DIAG_CAN_RX_OVERFLOW        0x0202
#define DIAG_CAN_WARNING            0x0203
#define DIAG_CAN_PASSIVE            0x0204
#define DIAG_CAN_BUS_OFF            0x0205
#define DIAG_CAN_RECOVERY_FAILED    0x0206
#define DIAG_TIME_STALE             0x0301
#define DIAG_SAMPLE_DEADLINE        0x0401
#define DIAG_SAMPLE_QUEUE_OVERFLOW  0x0402
#define DIAG_QUEUE_OVERFLOW         0x0403
#define DIAG_BEARING_TRANSITION     0x1101
#define DIAG_BEARING_RPM            0x1102
#define DIAG_ENGINE_REJECTED_PULSE  0x1201
#define DIAG_ENGINE_RPM             0x1202

#define CAN_ID_FRONT_BRAKE          0x0B1
#define CAN_ID_REAR_BRAKE           0x0B2
#define CAN_ID_BEARING_ENCODER      0x0B9
#define CAN_ID_GENERIC_ADC          0x0BA
#define CAN_ID_ENGINE_RPM           0x0BB

#define CAN_MASTER_TIME_RECORDING_FLAG (UINT64_C(1) << 63)
#define CAN_MASTER_TIME_VALUE_MASK     (CAN_MASTER_TIME_RECORDING_FLAG - 1)

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

static inline uint8_t protocol_sensor_node_id(uint32_t id)
{
    switch (id) {
        case CAN_ID_FRONT_BRAKE:
        case CAN_ID_REAR_BRAKE:     return 1;
        case CAN_ID_BEARING_ENCODER:return 4;
        case CAN_ID_ENGINE_RPM:     return 5;
        case CAN_ID_GENERIC_ADC:    return 6;
        default:                    return 0;
    }
}
