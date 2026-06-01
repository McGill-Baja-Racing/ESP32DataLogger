#include "frames.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <string.h>

// -------- PRIVATE DATA -------- //
uint8_t ping_data[1];
uint8_t control_msg_data[1];
uint8_t stop_data[1];
uint8_t fault_data[1];
uint8_t sensor_msg_data[8];

// -------- PRIVATE HELPERS -------- //
uint32_t get_node_id_from_frame_id(uint32_t frame_id) {
    return frame_id & CAN_NODE_MASK;
}
can_id_t get_cmd_id_from_frame_id(uint32_t frame_id) {
    return (can_id_t)(frame_id & CAN_CMD_MASK);
}

twai_frame_t create_twai_cmd_frame_for_node(can_id_t cmd_id, uint32_t node_id) {
    return (twai_frame_t){
        .header = {
            .id  = (cmd_id & CAN_CMD_MASK) | (node_id & CAN_NODE_MASK),
            .rtr = true,
        },
    };
}

// -------- NODE-SPECIFIC SENSOR FRAME -------- //
#if defined(NODE_ID) && NODE_ID == 1
    #define SENSOR_MSG_ID ID_Pa_FRONT_BRAKE
#elif defined(NODE_ID) && NODE_ID == 2
    #define SENSOR_MSG_ID ID_Pa_REAR_BRAKE
#elif defined(NODE_ID) && NODE_ID == 3
    #define SENSOR_MSG_ID ID_Temp_CVT
#elif defined(NODE_ID) && NODE_ID == 4
    #define SENSOR_MSG_ID ID_RPM_ENGINE
#elif defined(NODE_ID) && NODE_ID == 5
    #define SENSOR_MSG_ID ID_RPM_WHEEL
#endif

twai_frame_t sensor_frame_message = {
    .header     = { .id = SENSOR_MSG_ID, .ide = false },
    .buffer     = sensor_msg_data,
    .buffer_len = sizeof(sensor_msg_data),
};

static const twai_frame_t start_message = {
    .header = { .id = ID_START_CMD, .rtr = true },
};

static const twai_frame_t stop_message = {
    .header = { .id = ID_STOP_CMD, .rtr = true },
};

// -------- PUBLIC FUNCTIONS -------- //
void set_Sensor_id(Sensor *self, uint32_t message_id) {
    self->twai_frame = (twai_frame_t){
        .header     = { .id = message_id, .ide = false },
        .buffer     = self->msg_data,
        .buffer_len = sizeof(self->msg_data),
    };
}

void set_Sensor_data(Sensor *self, uint64_t data) {
    memcpy(self->msg_data, &data, sizeof(self->msg_data));
}