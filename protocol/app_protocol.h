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

#define CAN_PROTOCOL_VERSION        1

/* u8 target node (0xFF for all) | u8 update_op_t */
#define CAN_ID_UPDATE_COMMAND       0x0A4

typedef enum {
    UPDATE_OP_IDENTIFY = 1,
} update_op_t;

/* Node N sends node_identity_frame_t on CAN_ID_NODE_IDENTITY_BASE + N at boot
 * and in answer to UPDATE_OP_IDENTIFY. */
#define CAN_ID_NODE_IDENTITY_BASE   0x090
#define NODE_IDENTITY_HASH_LENGTH   6
#define NODE_IDENTITY_PENDING       0x01  /* image booted on trial, not yet confirmed */

typedef struct __attribute__((packed)) {
    uint8_t elf_sha256[NODE_IDENTITY_HASH_LENGTH];  /* start of esp_app_desc_t.app_elf_sha256 */
    uint8_t protocol_version;
    uint8_t flags;
} node_identity_frame_t;

/* Every image carries firmware_identity_t right after esp_app_desc_t: image
 * header (24) + first segment header (8) + esp_app_desc_t (256). */
#define FIRMWARE_IDENTITY_OFFSET    288
#define FIRMWARE_IDENTITY_MAGIC     0x42414A41

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t node_id;            /* 0 for the master */
    uint8_t protocol_version;
    uint8_t serial_test;        /* prints to serial and never joins the bus */
    uint8_t reserved;
} firmware_identity_t;

_Static_assert(sizeof(node_identity_frame_t) == 8, "identity must fill one CAN frame");

#define CAN_ID_NODE_STATUS_BASE     0x0D0
#define CAN_ID_NODE_EVENT_BASE      0x0E0
#define CAN_ID_NODE_DIAGNOSTICS_BASE 0x0F0

/*
 * Node health. Health frames use the sample layout, a 4-byte payload then a
 * uint32 timestamp_ms on the master clock, so the master logs them like
 * samples. Every node, and the master as node 0, sends a status frame and a
 * diagnostics frame each second; nodes send only their payload and the
 * master stamps arrival. While recording it also sends one event
 * frame per counted fault since the previous one. Status bits say what is
 * wrong now; events say how often and why. An event's timestamp is its last
 * occurrence, so events counted while idle reach the next log with their real
 * time. Subjects are one byte: every CAN ID on the bus is below 0x100. Late,
 * dropped, bad-timestamp, CAN TX and CAN RX overflow faults are counted per
 * subject, so one report can hold several events with the same code. The
 * master finds lost event frames from gaps in each node's event sequence.
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
     * fault is counted, except SENSOR_DEAD and NO_TIME_SYNC, which only
     * the status shows. */
    HEALTH_SENSOR_INIT,       /* event once: subject sensor CAN ID, error */
    HEALTH_SENSOR_READ,       /* event: subject sensor CAN ID, failed reads, last error */
    HEALTH_SENSOR_DEAD,       /* every read of a sensor failed */
    HEALTH_SAMPLES_MISSED,    /* event: subject sensor CAN ID, sample periods skipped */
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
    HEALTH_EVENT_LOST,        /* master; event: subject node ID, event frames lost */
    HEALTH_FAULT_COUNT,

    /* Events that are not faults. */
    HEALTH_BOOT = HEALTH_FAULT_COUNT,  /* subject: esp_reset_reason_t */
    HEALTH_ADC_UNCALIBRATED,           /* subject: GPIO, raw conversion used */
    HEALTH_NODE_OFFLINE,               /* master; subject: bit N set while node N is offline */
    HEALTH_CODE_COUNT,
} health_code_t;

#define HEALTH_BIT(code) (1U << (code))

_Static_assert(HEALTH_FAULT_COUNT <= 24, "faults must fit the 24 status bits");
_Static_assert(HEALTH_CODE_COUNT <= 32, "codes must fit the 5 event bits");

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
    HEALTH_ERROR_COUNT,
} health_error_t;

_Static_assert(HEALTH_ERROR_COUNT <= 16, "errors must fit the 4 event bits");

/* Faults are bits 0-23, health bits 24-25 and mode bit 26. */
typedef struct __attribute__((packed)) {
    uint32_t faults : 24;   /* HEALTH_BIT() of each fault */
    uint32_t health : 2;    /* node_health_t */
    uint32_t mode : 1;      /* node_mode_t */
    uint32_t : 5;
    uint32_t timestamp_ms;
} node_status_frame_t;

/* GCC packs bit-fields from bit 0 up: code is bits 0-4 of the first byte,
 * count the top 9 bits of the first uint32. */
typedef struct __attribute__((packed)) {
    uint32_t code : 5;      /* health_code_t */
    uint32_t error : 4;     /* health_error_t */
    uint32_t subject : 8;
    uint32_t sequence : 6;  /* node events sent since boot, wrapping; the boot
                             * event is sent first, so a reboot restarts at 0 */
    uint32_t count : 9;     /* occurrences since the previous event;
                             * HEALTH_EVENT_COUNT_MAX means that many or more */
    uint32_t timestamp_ms;
} health_event_frame_t;

#define HEALTH_EVENT_COUNT_MAX 0x1FF
#define HEALTH_EVENT_SEQUENCE_MASK 0x3F

typedef struct __attribute__((packed)) {
    uint8_t tx_errors;      /* TWAI transmit error counter, 255 or more at bus-off */
    uint8_t rx_errors;      /* TWAI receive error counter, capped at 255 */
    uint8_t resets;         /* resets since power-on, stops at 255 */
    uint8_t reset_reason;   /* esp_reset_reason_t of the last reset or power-on */
    uint32_t timestamp_ms;
} node_diagnostics_frame_t;

_Static_assert(sizeof(node_status_frame_t) == 8, "status must fill one CAN frame");
_Static_assert(sizeof(health_event_frame_t) == 8, "event must fill one CAN frame");
_Static_assert(sizeof(node_diagnostics_frame_t) == 8, "diagnostics must fill one CAN frame");

/* Status and diagnostics on the bus: a node that has just reset has no
 * master clock yet. */
#define NODE_HEALTH_PAYLOAD 4

static inline node_health_t health_for(uint32_t faults, bool no_sensor_works)
{
    const uint32_t error_faults =
        HEALTH_BIT(HEALTH_SENSOR_INIT) | HEALTH_BIT(HEALTH_SENSOR_DEAD) |
        HEALTH_BIT(HEALTH_BAD_TIMESTAMP) | HEALTH_BIT(HEALTH_NO_TIME_SYNC) |
        HEALTH_BIT(HEALTH_CAN_PASSIVE) | HEALTH_BIT(HEALTH_CAN_BUS_OFF) |
        HEALTH_BIT(HEALTH_SD_WRITE) | HEALTH_BIT(HEALTH_GPS_NO_DATA) |
        HEALTH_BIT(HEALTH_EVENT_LOST);
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

/* Health subjects are one byte, so health counts name the master-only GPS IDs
 * 0x70-0x72. */
#define HEALTH_SUBJECT_GPS_BASE 0x70

static inline uint32_t protocol_health_subject(uint32_t id)
{
    return id >= CAN_ID_GPS_SPEED && id <= CAN_ID_GPS_LONGITUDE
         ? id - CAN_ID_GPS_SPEED + HEALTH_SUBJECT_GPS_BASE : id;
}

static inline uint32_t protocol_health_subject_id(uint8_t subject)
{
    return subject >= HEALTH_SUBJECT_GPS_BASE &&
           subject <= HEALTH_SUBJECT_GPS_BASE + CAN_ID_GPS_LONGITUDE - CAN_ID_GPS_SPEED
         ? subject - HEALTH_SUBJECT_GPS_BASE + CAN_ID_GPS_SPEED : subject;
}
