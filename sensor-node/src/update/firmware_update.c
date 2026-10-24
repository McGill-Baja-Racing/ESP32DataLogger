#include "firmware_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "can/can_node.h"
#include "protocol/app_protocol.h"
#include "sdkconfig.h"

/* sdkconfig.defaults only fills options a stale sdkconfig.<env> lacks. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Rollback is off: delete sdkconfig.<env> and rebuild"
#endif

#ifndef SENSOR_SERIAL_TEST
#define SENSOR_SERIAL_TEST 0
#endif

static const char *TAG = "FirmwareUpdate";

const __attribute__((section(".rodata_custom_desc"))) firmware_identity_t firmware_identity = {
    .magic = FIRMWARE_IDENTITY_MAGIC,
    .node_id = NODE_ID,
    .protocol_version = CAN_PROTOCOL_VERSION,
    .serial_test = SENSOR_SERIAL_TEST,
};

void firmware_update_report_identity(void)
{
    node_identity_frame_t frame = {
        .protocol_version = CAN_PROTOCOL_VERSION,
    };
    memcpy(frame.elf_sha256, esp_app_get_description()->app_elf_sha256,
           NODE_IDENTITY_HASH_LENGTH);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        frame.flags |= NODE_IDENTITY_PENDING;
    }
    esp_err_t error = can_node_send(CAN_ID_NODE_IDENTITY_BASE + NODE_ID,
                                    (const uint8_t *)&frame, sizeof(frame));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Identity report failed: %s", esp_err_to_name(error));
    }
}

void firmware_update_handle_command(const uint8_t *payload, uint8_t length)
{
    if (length >= 2 && payload[1] == UPDATE_OP_IDENTIFY) {
        firmware_update_report_identity();
    }
}
