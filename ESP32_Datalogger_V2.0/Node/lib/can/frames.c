#include "esp_err.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"


/* Naming Convention
    ID: ID_X_CMD
    Frame: X_message
*/
// -------- COMMAND IDS ------- //
typedef enum{
    ID_CONTROL_CMD        = 0x0A4,
    ID_START_CMD          = 0x0A5,
    ID_STOP_CMD           = 0x0A6,
    ID_ERROR_CMD          = 0xF00,
    ID_MASTER_TIME_BEACON = 0x0A2,
    ID_RPM_DATA           = 0x0D0,

} can_id_t;




static uint8_t  ping_data[1];
static uint8_t  control_msg_data[1];
static uint8_t rpm_msg_data[8];

static uint8_t stop_data[1];
static uint8_t fault_data[1];


// -------- FRAMES ------- //

static const twai_frame_t control_message = {
    .header = {
        .id = ID_CONTROL_CMD,
        .ide = false,
    },
    .buffer = control_msg_data,
    .buffer_len = 0,
};

static const twai_frame_t rpm_message = {
    .header = {
        .id = ID_RPM_DATA,
        .ide = false,
    },
    .buffer = rpm_msg_data,
    .buffer_len = sizeof(rpm_msg_data), // timestamp + data
};

static const twai_frame_t start_message = {
    .header = {
        .id = ID_CONTROL_CMD,
        .rtr = true,
    },
};
static const twai_frame_t stop_message = {
    .header = {
        .id = ID_STOP_CMD,
        .rtr = true,
    },
};



