#include "firmware_update.h"

#include <dirent.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"
#include "sdkconfig.h"

#ifndef CAN_PROTOCOL_HASH
#error "CAN_PROTOCOL_HASH comes from protocol/protocol_hash.py"
#endif

/* sdkconfig.defaults only fills options a stale sdkconfig.<env> lacks. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Rollback is off: delete sdkconfig.<env> and rebuild"
#endif

#define FIRMWARE_DIR              "/sdcard/firmware"
#define UPDATE_REPLY_TIMEOUT_MS   10000
#define UPDATE_CONFIRM_WAIT_MS    30000
#define IDENTITY_WAIT_MS          500
#define RESULT_LENGTH             48
#define MASTER_WRITE_BYTES        4096

static const char *TAG = "FirmwareUpdate";
static QueueHandle_t reply_queue;

const __attribute__((section(".rodata_custom_desc"))) firmware_identity_t firmware_identity = {
    .magic = FIRMWARE_IDENTITY_MAGIC,
    .node_id = 0,
    .serial_test = 0,
    .protocol_hash = CAN_PROTOCOL_HASH,
};

static struct {
    bool seen;
    uint32_t reports;
    node_identity_frame_t frame;
} nodes[CAN_NODE_SLOT_COUNT];

typedef struct {
    bool present;
    bool duplicate;
    char path[96];
    uint32_t size;
    uint8_t hash[NODE_IDENTITY_HASH_LENGTH];
} image_t;

static const char *const error_names[UPDATE_ERROR_COUNT] = {
    "none", "recording", "too large", "on trial", "wrong image", "flash", "bad image", "timeout",
};

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
        ESP_LOGI(TAG, "Node %u firmware %s protocol %04x%s", id, hash,
                 frame.protocol_hash,
                 (frame.flags & NODE_IDENTITY_PENDING) ? " (on trial)" : "");
        if (frame.protocol_hash != CAN_PROTOCOL_HASH) {
            ESP_LOGW(TAG, "Node %u protocol %04x, master %04x", id,
                     frame.protocol_hash, CAN_PROTOCOL_HASH);
        }
    }
    nodes[id].frame = frame;
    nodes[id].seen = true;
    nodes[id].reports++;
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

esp_err_t firmware_update_init(void)
{
    reply_queue = xQueueCreate(16, sizeof(can_message_t));
    return reply_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

bool firmware_update_is_reply_frame(const can_message_t *message)
{
    return message->id > CAN_ID_NODE_UPDATE_BASE &&
           message->id < CAN_ID_NODE_UPDATE_BASE + CAN_NODE_SLOT_COUNT;
}

void firmware_update_handle_reply(const can_message_t *message)
{
    (void)xQueueSend(reply_queue, message, 0);
}

#define IMAGE_MAGIC               0xE9
#define IMAGE_APP_DESC_OFFSET     32
#define APP_DESC_MAGIC            0xABCD5432
#define APP_DESC_ELF_SHA256_OFFSET 144

static bool parse_image(const uint8_t *header, size_t length, firmware_identity_t *identity,
                        uint8_t *hash)
{
    if (length < FIRMWARE_IDENTITY_OFFSET + sizeof(*identity) || header[0] != IMAGE_MAGIC) {
        return false;
    }
    const uint8_t *desc = header + IMAGE_APP_DESC_OFFSET;
    uint32_t desc_magic = (uint32_t)desc[0] | (uint32_t)desc[1] << 8 |
                          (uint32_t)desc[2] << 16 | (uint32_t)desc[3] << 24;
    if (desc_magic != APP_DESC_MAGIC) {
        return false;
    }
    memcpy(identity, header + FIRMWARE_IDENTITY_OFFSET, sizeof(*identity));
    if (identity->magic != FIRMWARE_IDENTITY_MAGIC) {
        return false;
    }
    memcpy(hash, desc + APP_DESC_ELF_SHA256_OFFSET, NODE_IDENTITY_HASH_LENGTH);
    return true;
}

_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) == APP_DESC_ELF_SHA256_OFFSET,
               "image hash offset must match esp_app_desc_t");
_Static_assert(sizeof(esp_app_desc_t) + IMAGE_APP_DESC_OFFSET == FIRMWARE_IDENTITY_OFFSET,
               "identity must follow esp_app_desc_t");

static void scan_images(image_t *images)
{
    DIR *directory = opendir(FIRMWARE_DIR);
    if (!directory) {
        ESP_LOGW(TAG, "No " FIRMWARE_DIR " directory");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        if (name_length < 4 || strcasecmp(entry->d_name + name_length - 4, ".bin") != 0) {
            continue;
        }
        char path[96];
        if (snprintf(path, sizeof(path), FIRMWARE_DIR "/%s", entry->d_name) >= (int)sizeof(path)) {
            ESP_LOGW(TAG, "Skipping %s: name too long", entry->d_name);
            continue;
        }
        uint8_t header[FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t)];
        struct stat info;
        FILE *file = fopen(path, "rb");
        bool read_ok = file && stat(path, &info) == 0 &&
                       fread(header, 1, sizeof(header), file) == sizeof(header);
        if (file) {
            fclose(file);
        }
        firmware_identity_t identity;
        uint8_t hash[NODE_IDENTITY_HASH_LENGTH];
        if (!read_ok || !parse_image(header, sizeof(header), &identity, hash)) {
            ESP_LOGW(TAG, "Skipping %s: not a firmware image", path);
        } else if (identity.serial_test) {
            ESP_LOGW(TAG, "Skipping %s: serial-test image", path);
        } else if (identity.node_id != 0 &&
                   (identity.node_id >= CAN_NODE_SLOT_COUNT ||
                    !(CAN_NODE_ID_MASK & (1U << identity.node_id)))) {
            ESP_LOGW(TAG, "Skipping %s: unknown node %u", path, identity.node_id);
        } else if (images[identity.node_id].present) {
            images[identity.node_id].duplicate = true;
        } else {
            image_t *image = &images[identity.node_id];
            image->present = true;
            strcpy(image->path, path);
            image->size = (uint32_t)info.st_size;
            memcpy(image->hash, hash, sizeof(hash));
        }
    }
    closedir(directory);
}

static void send_abort(unsigned int id)
{
    uint8_t payload[] = {(uint8_t)id, UPDATE_OP_ABORT};
    (void)can_master_send(CAN_ID_UPDATE_COMMAND, payload, sizeof(payload));
}

static void send_chunk(FILE *file, uint32_t offset, uint32_t length, uint8_t tag, bool *read_ok)
{
    uint8_t chunk[UPDATE_CHUNK_BYTES];
    *read_ok = fseek(file, offset, SEEK_SET) == 0 && fread(chunk, 1, length, file) == length;
    if (!*read_ok) {
        return;
    }
    for (uint32_t i = 0; i * UPDATE_FRAME_BYTES < length; i++) {
        uint32_t bytes = length - i * UPDATE_FRAME_BYTES;
        if (bytes > UPDATE_FRAME_BYTES) {
            bytes = UPDATE_FRAME_BYTES;
        }
        uint8_t payload[1 + UPDATE_FRAME_BYTES];
        payload[0] = (uint8_t)(tag << UPDATE_TAG_SHIFT) | (uint8_t)i;
        memcpy(payload + 1, chunk + i * UPDATE_FRAME_BYTES, bytes);
        (void)can_master_send(CAN_ID_UPDATE_DATA, payload, (uint8_t)(1 + bytes));
    }
}

static bool transfer_image(FILE *file, unsigned int id, const image_t *image, char *result)
{
    xQueueReset(reply_queue);
    uint8_t begin[] = {
        (uint8_t)id, UPDATE_OP_BEGIN,
        (uint8_t)image->size, (uint8_t)(image->size >> 8),
        (uint8_t)(image->size >> 16), (uint8_t)(image->size >> 24),
    };
    esp_err_t error = can_master_send(CAN_ID_UPDATE_COMMAND, begin, sizeof(begin));
    if (error != ESP_OK) {
        snprintf(result, RESULT_LENGTH, "failed: CAN send");
        return false;
    }
    unsigned int next_progress = 25;
    bool last_chunk_sent = false;
    while (true) {
        can_message_t reply;
        if (xQueueReceive(reply_queue, &reply, pdMS_TO_TICKS(UPDATE_REPLY_TIMEOUT_MS)) != pdTRUE) {
            if (last_chunk_sent) {
                /* DONE was lost; the confirmation wait shows whether the node rebooted. */
                return true;
            }
            send_abort(id);
            snprintf(result, RESULT_LENGTH, "failed: no reply");
            return false;
        }
        /* Serve only the newest reply: a retry READ answered late can carry
         * the 3-bit tag the node expects for its next offset. */
        can_message_t newer;
        while (xQueueReceive(reply_queue, &newer, 0) == pdTRUE) {
            if (newer.id == CAN_ID_NODE_UPDATE_BASE + id) {
                reply = newer;
            }
        }
        if (reply.id - CAN_ID_NODE_UPDATE_BASE != id || reply.dlc < 1) {
            continue;
        }
        uint8_t kind = (uint8_t)reply.data;
        if (kind == UPDATE_REPLY_DONE) {
            return true;
        }
        if (kind == UPDATE_REPLY_ERROR && reply.dlc >= 2) {
            uint8_t code = (uint8_t)(reply.data >> 8);
            snprintf(result, RESULT_LENGTH, "failed: %s",
                     code < UPDATE_ERROR_COUNT ? error_names[code] : "unknown error");
            return false;
        }
        if (kind != UPDATE_REPLY_READ || reply.dlc < 6) {
            continue;
        }
        uint32_t offset = (uint32_t)(reply.data >> 8);
        uint8_t tag = (uint8_t)(reply.data >> 40);
        if (offset >= image->size) {
            send_abort(id);
            snprintf(result, RESULT_LENGTH, "failed: bad offset");
            return false;
        }
        uint32_t length = image->size - offset;
        if (length > UPDATE_CHUNK_BYTES) {
            length = UPDATE_CHUNK_BYTES;
        }
        bool read_ok;
        send_chunk(file, offset, length, tag, &read_ok);
        if (!read_ok) {
            send_abort(id);
            snprintf(result, RESULT_LENGTH, "failed: SD read");
            return false;
        }
        last_chunk_sent = offset + length == image->size;
        if ((uint64_t)offset * 100 >= (uint64_t)image->size * next_progress) {
            ESP_LOGI(TAG, "Node %u: %u%%", id, next_progress);
            next_progress += 25;
        }
    }
}

static void flash_node(unsigned int id, const image_t *image, char *result)
{
    FILE *file = fopen(image->path, "rb");
    if (!file) {
        snprintf(result, RESULT_LENGTH, "failed: cannot open image");
        return;
    }
    bool sent = transfer_image(file, id, image, result);
    fclose(file);
    if (!sent) {
        return;
    }
    for (int waited = 0; waited < UPDATE_CONFIRM_WAIT_MS; waited += 100) {
        if (waited % 2000 == 0) {
            (void)firmware_update_request_identity();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (memcmp(nodes[id].frame.elf_sha256, image->hash, NODE_IDENTITY_HASH_LENGTH) == 0 &&
            !(nodes[id].frame.flags & NODE_IDENTITY_PENDING)) {
            snprintf(result, RESULT_LENGTH, "updated");
            return;
        }
    }
    snprintf(result, RESULT_LENGTH, "failed: new image not confirmed");
}

/* True once the new image is set to boot. */
static bool update_master(const image_t *image, bool node_failed, char *result)
{
    if (!image->present) {
        snprintf(result, RESULT_LENGTH, "no image");
        return false;
    }
    if (image->duplicate) {
        snprintf(result, RESULT_LENGTH, "two images, skipped");
        return false;
    }
    if (node_failed) {
        snprintf(result, RESULT_LENGTH, "skipped: a node update failed");
        return false;
    }
    if (memcmp(esp_app_get_description()->app_elf_sha256, image->hash,
               NODE_IDENTITY_HASH_LENGTH) == 0) {
        snprintf(result, RESULT_LENGTH, "current");
        return false;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || image->size > part->size) {
        snprintf(result, RESULT_LENGTH, "failed: image too large");
        return false;
    }
    esp_ota_handle_t handle;
    esp_err_t error = esp_ota_begin(part, image->size, &handle);
    if (error != ESP_OK) {
        snprintf(result, RESULT_LENGTH, error == ESP_ERR_OTA_ROLLBACK_INVALID_STATE
                                            ? "failed: running image not confirmed"
                                            : "failed: flash");
        return false;
    }
    FILE *file = fopen(image->path, "rb");
    if (!file) {
        (void)esp_ota_abort(handle);
        snprintf(result, RESULT_LENGTH, "failed: cannot open image");
        return false;
    }
    ESP_LOGI(TAG, "Master: flashing %s", image->path);
    static uint8_t buffer[MASTER_WRITE_BYTES];
    uint32_t offset = 0;
    unsigned int next_progress = 25;
    while (offset < image->size) {
        size_t length = image->size - offset;
        if (length > sizeof(buffer)) {
            length = sizeof(buffer);
        }
        if (fread(buffer, 1, length, file) != length) {
            (void)esp_ota_abort(handle);
            fclose(file);
            snprintf(result, RESULT_LENGTH, "failed: SD read");
            return false;
        }
        if (esp_ota_write(handle, buffer, length) != ESP_OK) {
            (void)esp_ota_abort(handle);
            fclose(file);
            snprintf(result, RESULT_LENGTH, "failed: flash");
            return false;
        }
        offset += length;
        if ((uint64_t)offset * 100 >= (uint64_t)image->size * next_progress) {
            ESP_LOGI(TAG, "Master: %u%%", next_progress);
            next_progress += 25;
        }
    }
    fclose(file);
    error = esp_ota_end(handle);
    if (error != ESP_OK) {
        snprintf(result, RESULT_LENGTH, error == ESP_ERR_OTA_VALIDATE_FAILED
                                            ? "failed: bad image"
                                            : "failed: flash");
        return false;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        snprintf(result, RESULT_LENGTH, "failed: flash");
        return false;
    }
    snprintf(result, RESULT_LENGTH, "updated; restarting");
    return true;
}

void firmware_update_run(void)
{
    static image_t images[CAN_NODE_SLOT_COUNT];
    memset(images, 0, sizeof(images));
    scan_images(images);

    uint32_t reports[CAN_NODE_SLOT_COUNT];
    for (unsigned int id = 0; id < CAN_NODE_SLOT_COUNT; id++) {
        reports[id] = nodes[id].reports;
    }
    (void)firmware_update_request_identity();
    vTaskDelay(pdMS_TO_TICKS(IDENTITY_WAIT_MS));

    char results[CAN_NODE_SLOT_COUNT][RESULT_LENGTH];
    for (unsigned int id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if (!(CAN_NODE_ID_MASK & (1U << id))) {
            continue;
        }
        if (!images[id].present) {
            snprintf(results[id], RESULT_LENGTH, "no image");
        } else if (images[id].duplicate) {
            snprintf(results[id], RESULT_LENGTH, "two images, skipped");
        } else if (nodes[id].reports == reports[id]) {
            snprintf(results[id], RESULT_LENGTH, "offline");
        } else if (memcmp(nodes[id].frame.elf_sha256, images[id].hash,
                          NODE_IDENTITY_HASH_LENGTH) == 0) {
            snprintf(results[id], RESULT_LENGTH, "current");
        } else {
            ESP_LOGI(TAG, "Node %u: flashing %s", id, images[id].path);
            flash_node(id, &images[id], results[id]);
        }
    }
    for (unsigned int id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if (CAN_NODE_ID_MASK & (1U << id)) {
            ESP_LOGI(TAG, "Node %u: %s", id, results[id]);
        }
    }

    bool node_failed = false;
    for (unsigned int id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if ((CAN_NODE_ID_MASK & (1U << id)) && strncmp(results[id], "failed", 6) == 0) {
            node_failed = true;
        }
    }
    char master_result[RESULT_LENGTH];
    bool restart = update_master(&images[0], node_failed, master_result);
    ESP_LOGI(TAG, "Master: %s", master_result);
    if (restart) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

void firmware_update_confirm(void)
{
    if (!is_on_trial()) {
        return;
    }
    esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "New image confirmed");
    } else {
        ESP_LOGW(TAG, "Cannot confirm new image: %s", esp_err_to_name(error));
    }
}
