#include "esp_err.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

/* Naming Convention
    ID: ID_X_CMD
    Frame: X_message
*/

// -------- COMMAND IDS ------- //
typedef enum {
    ID_CONTROL_CMD        =   0x0A1,
    ID_ERROR_CMD          =   0xF00,
    ID_MASTER_TIME_BEACON =   0x0A2,
} can_id_t;

typedef enum {
    START_CMD = 1,
    STOP_CMD  = 0,
} control_cmd_t;


static uint8_t ping_data[1];
static uint8_t control_msg_data[1];
static uint8_t stop_data[1];
static uint8_t fault_data[1];


// -------- FRAMES ------- //
static const twai_frame_t control_message = {
    .header.id = ID_CONTROL_CMD,    // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = control_msg_data,         // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

static const twai_frame_t error_message = {
    .header.id = ID_ERROR_CMD,     // Message ID
    .header.ide = false,            // Use 29-bit extended ID format
    .buffer = fault_data,                  // Pointer to data to transmit
    .buffer_len = 0,                // Length of data to transmit
};

