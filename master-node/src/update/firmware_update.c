#include "firmware_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "protocol/app_protocol.h"
#include "sdkconfig.h"

/* sdkconfig.defaults only fills options a stale sdkconfig.<env> lacks. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Rollback is off: delete sdkconfig.<env> and rebuild"
#endif

static const char *TAG = "FirmwareUpdate";

const __attribute__((section(".rodata_custom_desc"))) firmware_identity_t firmware_identity = {
    .magic = FIRMWARE_IDENTITY_MAGIC,
    .node_id = 0,
    .protocol_version = CAN_PROTOCOL_VERSION,
    .serial_test = 0,
};

static struct {
    bool seen;
    node_identity_frame_t frame;
} nodes[CAN_NODE_SLOT_COUNT];

static void format_hash(const uint8_t *hash, char *text)
{
    for (int i = 0; i < NODE_IDENTITY_HASH_LENGTH; i++) {
        sprintf(text + 2 * i, "%02x", hash[i]);
    }
}

bool firmware_update_is_identity_frame(const can_message_t *message)
{
    return message->id > CAN_ID_NODE_IDENTITY_BASE &&
           message->id < CAN_ID_NODE_IDENTITY_BASE + CAN_NODE_SLOT_COUNT;
}

void firmware_update_handle_identity(const can_message_t *message)
{
    if (message->dlc != 8) {
        return;
    }
    unsigned int id = message->id - CAN_ID_NODE_IDENTITY_BASE;
    node_identity_frame_t frame;
    memcpy(&frame, &message->data, sizeof(frame));
    if (!nodes[id].seen || memcmp(&nodes[id].frame, &frame, sizeof(frame)) != 0) {
        char hash[2 * NODE_IDENTITY_HASH_LENGTH + 1];
        format_hash(frame.elf_sha256, hash);
        ESP_LOGI(TAG, "Node %u firmware %s protocol %u%s", id, hash,
                 frame.protocol_version,
                 (frame.flags & NODE_IDENTITY_PENDING) ? " (on trial)" : "");
        if (frame.protocol_version != CAN_PROTOCOL_VERSION) {
            ESP_LOGW(TAG, "Node %u speaks protocol %u, master %u", id,
                     frame.protocol_version, CAN_PROTOCOL_VERSION);
        }
    }
    nodes[id].frame = frame;
    nodes[id].seen = true;
}

esp_err_t firmware_update_request_identity(void)
{
    uint8_t payload[] = {0xFF, UPDATE_OP_IDENTIFY};
    return can_master_send(CAN_ID_UPDATE_COMMAND, payload, sizeof(payload));
}

static bool is_on_trial(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void firmware_update_print_identity(void)
{
    char line[192];
    char hash[2 * NODE_IDENTITY_HASH_LENGTH + 1];
    format_hash(esp_app_get_description()->app_elf_sha256, hash);
    int length = snprintf(line, sizeof(line), "master=%s%s", hash,
                          is_on_trial() ? " (on trial)" : "");
    for (unsigned int id = 1;
         MASTER_CAN_ENABLED && id < CAN_NODE_SLOT_COUNT && length < (int)sizeof(line); id++) {
        if (!(CAN_NODE_ID_MASK & (1U << id))) {
            continue;
        }
        if (!nodes[id].seen) {
            length += snprintf(line + length, sizeof(line) - length, " %u=unknown", id);
            continue;
        }
        format_hash(nodes[id].frame.elf_sha256, hash);
        length += snprintf(line + length, sizeof(line) - length, " %u=%s%s", id, hash,
                           (nodes[id].frame.flags & NODE_IDENTITY_PENDING) ? " (on trial)" : "");
    }
    ESP_LOGI(TAG, "firmware: %s", line);
}
