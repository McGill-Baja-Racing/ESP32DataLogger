#include "esp_err.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"


#define CAN_NODE_MASK   0x00F   // 4 bits for node
#define CAN_CMD_MASK    0xFF0  // upper bits

/* Naming Convention
    ID: ID_X_CMD
    Frame: X_message
*/




typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint64_t data;
} can_rx_word_t;


// -------- COMMAND IDS ------- //
/* 
SIDE NOTE: Lowest IDS --> higher priority???
C --> for Commands --> last bit is node ID C1# where # is the node id
E --> for Errors --> # can do the same thing with Errors # is the id of the node with the error
D --> for Data
*/

typedef enum{
    ID_REBOOT_CMD         = 0xC00,
    ID_START_CMD          = 0xC10,
    ID_STOP_CMD           = 0xC20,
    ID_ERROR_CMD          = 0xE00,
    ID_MASTER_TIME_BEACON = 0xD00,
    ID_RPM_ENGINE         = 0xD01,
    ID_RPM_WHEEL          = 0xD02,
    ID_Pa_FRONT_BRAKE     = 0xD03,
    ID_Pa_REAR_BRAKE      = 0xD04,
    ID_Temp_CVT           = 0xD05

} can_id_t;

static uint8_t  ping_data[1];
static uint8_t  control_msg_data[1];
static uint8_t stop_data[1];
static uint8_t fault_data[1];

static uint8_t  rpm_msg_eng[8];
static uint8_t  rpm_msg_wheel[8];
static uint8_t  pa_msg_front_brake[8];
static uint8_t  pa_msg_rear_brake[8];
static uint8_t  temp_msg_cvt[8];




// -------- FRAMES ------- //

static twai_frame_t create_twai_cmd_frame_for_node(can_id_t cmd_id, uint32_t node_id)
{
    return (twai_frame_t){
        .header = {
            .id = (cmd_id & CAN_CMD_MASK) | (node_id & CAN_NODE_MASK),
            .rtr = true,
        },
    };
}

static const twai_frame_t rpm_eng_message = {
    .header = {
        .id = ID_RPM_ENGINE,
        .ide = false,
    },
    .buffer = rpm_msg_eng,
    .buffer_len = sizeof(rpm_msg_eng), // timestamp + data
};


static const twai_frame_t rpm_wheel_message = {
    .header = {
        .id = ID_RPM_WHEEL,
        .ide = false,
    },
    .buffer = rpm_msg_wheel,
    .buffer_len = sizeof(rpm_msg_wheel), // timestamp + data
};

static const twai_frame_t pa_front_brake_message = {
    .header = {
        .id = ID_Pa_FRONT_BRAKE,
        .ide = false,
    },
    .buffer = pa_msg_front_brake,
    .buffer_len = sizeof(pa_msg_front_brake), // timestamp + data
};

static const twai_frame_t pa_rear_brake_message = {
    .header = {
        .id = ID_Pa_REAR_BRAKE,
        .ide = false,
    },
    .buffer = pa_msg_rear_brake,
    .buffer_len = sizeof(pa_msg_rear_brake), // timestamp + data
};

static const twai_frame_t temp_cvt_message = {
    .header = {
        .id = ID_Temp_CVT,
        .ide = false,
    },
    .buffer = temp_msg_cvt,
    .buffer_len = sizeof(temp_msg_cvt), // timestamp + data
};


static const twai_frame_t start_message = {
    .header = {
        .id = ID_START_CMD,
        .rtr = true,
    },
};
static const twai_frame_t stop_message = {
    .header = {
        .id = ID_STOP_CMD,
        .rtr = true,
    },
};



