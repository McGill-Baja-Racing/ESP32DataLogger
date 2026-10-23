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
#define CAN_NODE_ID_MASK            ((1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6))
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

#define CAN_ID_NODE_STATUS_BASE     0x0D0
#define CAN_ID_NODE_EVENT_BASE      0x0E0

/*
 * Node health. Health frames use the sample layout, a 4-byte payload then a
 * uint32 timestamp_ms on the master clock, so the master logs them like
 * samples. Every node, and the master as node 0, sends a status frame each
 * second. While recording it also sends one event frame per counted fault
 * since the previous one. Status bits say what is wrong now; events say how
 * often and why. An event's timestamp is its last occurrence, so events counted
 * while idle reach the next log with their real time. Subjects are one byte:
 * every CAN ID on the bus is below 0x100.
 */
#define NODE_STATUS_PERIOD_MS 1000

typedef enum {
    NODE_HEALTH_OK = 0,
    NODE_HEALTH_WARNING = 1,   /* data lost, node still working */
    NODE_HEALTH_ERROR = 2,     /* a sensor, the clock, CAN or the SD card is failing */
    NODE_HEALTH_CRITICAL = 3,  /* no sensor produces data */
} node_health_t;

typedef enum {
    NODE_MODE_IDLE = 0,
    NODE_MODE_RECORDING = 1,
} node_mode_t;

typedef enum {
    /* Faults. Status bit N is set while fault N holds or if it was
     * counted during the last status period. Each onset of a lasting
     * fault is counted. */
    HEALTH_SENSOR_INIT,       /* event once: subject sensor CAN ID, error */
    HEALTH_SENSOR_READ,       /* event: subject sensor CAN ID, failed reads, last error */
    HEALTH_SENSOR_DEAD,       /* every read of a sensor failed */
    HEALTH_SAMPLES_LATE,      /* event: subject sensor CAN ID, sample periods skipped */
    HEALTH_SAMPLES_DROPPED,   /* event: subject CAN ID, samples or spark captures lost */
    HEALTH_BAD_TIMESTAMP,     /* event: subject CAN ID, negative master time */
    HEALTH_NO_TIME_SYNC,      /* no master time beacon for over 1 s */
    HEALTH_CAN_TX,            /* event: subject CAN ID of the frame not sent, error */
    HEALTH_CAN_RX_OVERFLOW,   /* event: subject CAN ID of the frame lost */
    HEALTH_CAN_NO_ACK,        /* event: open wire or no other node */
    HEALTH_CAN_BUS_ERROR,     /* event: bit, stuff, form or CRC error, noise or short */
    HEALTH_CAN_PASSIVE,
    HEALTH_CAN_BUS_OFF,       /* event: error set when recovery could not start */
    HEALTH_SD_WRITE,          /* master; a write failed or a log file is lost */
    HEALTH_GPS_NO_DATA,       /* master; no NMEA bytes for 3 s */
    HEALTH_GPS_NO_FIX,        /* master; no position fix for 3 s */
    HEALTH_FAULT_COUNT,

    /* Events that are not faults. */
    HEALTH_BOOT = HEALTH_FAULT_COUNT,  /* subject: esp_reset_reason_t */
    HEALTH_ADC_UNCALIBRATED,           /* subject: GPIO, raw conversion used */
    HEALTH_CODE_COUNT,
} health_code_t;

#define HEALTH_BIT(code) (1U << (code))

_Static_assert(HEALTH_FAULT_COUNT <= 16, "faults must fit the 16 status bits");

/* An event's error: one entry per esp_err_t the firmware returns. */
typedef enum {
    HEALTH_ERROR_NONE,
    HEALTH_ERROR_FAIL,
    HEALTH_ERROR_NO_MEM,
    HEALTH_ERROR_INVALID_ARG,
    HEALTH_ERROR_INVALID_STATE,
    HEALTH_ERROR_NOT_FOUND,
    HEALTH_ERROR_TIMEOUT,
    HEALTH_ERROR_INVALID_RESPONSE,
    HEALTH_ERROR_OTHER,
} health_error_t;

typedef struct __attribute__((packed)) {
    uint16_t faults;        /* HEALTH_BIT() of each fault */
    uint8_t health;         /* node_health_t */
    uint8_t mode;           /* node_mode_t */
    uint32_t timestamp_ms;
} node_status_frame_t;

typedef struct __attribute__((packed)) {
    uint8_t code;           /* health_code_t */
    uint8_t subject;
    uint8_t count;          /* occurrences since the previous event; 255 means 255 or more */
    uint8_t error;          /* health_error_t */
    uint32_t timestamp_ms;
} health_event_frame_t;

_Static_assert(sizeof(node_status_frame_t) == 8, "status must fill one CAN frame");
_Static_assert(sizeof(health_event_frame_t) == 8, "event must fill one CAN frame");

static inline node_health_t health_for(uint16_t faults, bool no_sensor_works)
{
    const uint16_t error_faults =
        HEALTH_BIT(HEALTH_SENSOR_INIT) | HEALTH_BIT(HEALTH_SENSOR_DEAD) |
        HEALTH_BIT(HEALTH_BAD_TIMESTAMP) | HEALTH_BIT(HEALTH_NO_TIME_SYNC) |
        HEALTH_BIT(HEALTH_CAN_PASSIVE) | HEALTH_BIT(HEALTH_CAN_BUS_OFF) |
        HEALTH_BIT(HEALTH_SD_WRITE) | HEALTH_BIT(HEALTH_GPS_NO_DATA);
    if (no_sensor_works) {
        return NODE_HEALTH_CRITICAL;
    }
    if (faults & error_faults) {
        return NODE_HEALTH_ERROR;
    }
    if (faults) {
        return NODE_HEALTH_WARNING;
    }
    return NODE_HEALTH_OK;
}

/* Node that sends a sample frame (int32 value, uint32 timestamp_ms); 0 for any other ID. */
static inline uint8_t protocol_sensor_node_id(uint32_t id)
{
    switch (id) {
        case CAN_ID_FRONT_BRAKE:
        case CAN_ID_REAR_BRAKE:     return 1;
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
