#pragma once

#include <stdint.h>
#include "esp_twai.h"

// -------- MASKS -------- //
#define CAN_NODE_MASK  0x00F   // 4 bits for node
#define CAN_CMD_MASK   0xFF0   // upper bits

// -------- TYPES -------- //
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;

typedef enum {
    ID_REBOOT_CMD         = 0xC00,
    ID_START_CMD          = 0xC10,
    ID_STOP_CMD           = 0xC20,
    ID_MASTER_TIME_BEACON = 0xC30,
    ID_ERROR_CMD          = 0xE00,
    ID_Pa_FRONT_BRAKE     = 0xD01,
    ID_Pa_REAR_BRAKE      = 0xD02,
    ID_Temp_CVT           = 0xD03,
    ID_GPS                = 0xD04,
    ID_ACCELOROMETER      = 0xD05,
    ID_RPM_ENGINE         = 0xD06,
    ID_RPM_WHEEL          = 0xD07,
} can_id_t;

typedef struct {
    twai_frame_t twai_frame;
    uint8_t      msg_data[8];
} Sensor;

// Important need to wrap C functions because CPP messes them up
#ifdef __cplusplus
extern "C" {
#endif

// -------- PUBLIC VARIABLES -------- //
extern uint8_t ping_data[1];
extern uint8_t control_msg_data[1];
extern uint8_t stop_data[1];
extern uint8_t fault_data[1];
extern uint8_t sensor_msg_data[8];
extern twai_frame_t sensor_frame_message;

// -------- PUBLIC FUNCTIONS -------- //
void         set_Sensor_id(Sensor *self, uint32_t message_id);
void         set_Sensor_data(Sensor *self, uint64_t data);
uint32_t     get_node_id_from_frame_id(uint32_t frame_id);
can_id_t     get_cmd_id_from_frame_id(uint32_t frame_id);
twai_frame_t create_twai_cmd_frame_for_node(can_id_t cmd_id, uint32_t node_id);

#ifdef __cplusplus
}
#endif
