#include "firmware_update.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "node_state/node_registry.h"
#include "protocol/app_protocol.h"
#include "sdkconfig.h"

#ifndef CAN_PROTOCOL_HASH
#error "CAN_PROTOCOL_HASH comes from protocol/build_hooks.py"
#endif

/* sdkconfig.defaults only fills options a stale sdkconfig.<env> lacks. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Rollback is off: delete sdkconfig.<env> and rebuild"
#endif

#define UPDATE_REPLY_TIMEOUT_MS   10000
#define UPDATE_CONFIRM_WAIT_MS    30000
#define IDENTITY_WAIT_MS          500
#define MASTER_RESTART_DELAY_MS   2000
#define IDENTITY_POLL_MS          100
#define IDENTITY_REQUEST_MS       2000
#define PATH_BYTES                96
#define MASTER_WRITE_BYTES        4096
#define MASTER_CONFIRM_TIMEOUT_MS 60000
#define RESUME_PATH               FIRMWARE_DIR "/resume"

static const char *TAG = "FirmwareUpdate";
static QueueHandle_t reply_queue;

static volatile bool running;
static bool resume;
static volatile int target = -1;
static volatile unsigned int percent;
/* Slot 0 is the master. */
static const char *results[CAN_NODE_SLOT_COUNT];

const __attribute__((section(".rodata_custom_desc"))) firmware_identity_t firmware_identity = {
    .magic = FIRMWARE_IDENTITY_MAGIC,
    .node_id = 0,
    .bench_only = !MASTER_CAN_ENABLED,
    .protocol_hash = CAN_PROTOCOL_HASH,
};

static struct {
    int64_t heard_us;
    node_identity_frame_t frame;
} nodes[CAN_NODE_SLOT_COUNT];

typedef struct {
    bool present;
    char path[PATH_BYTES];
    uint32_t size;
    uint8_t hash[NODE_IDENTITY_HASH_LENGTH];
    uint16_t protocol_hash;
} image_t;

static image_t images[CAN_NODE_SLOT_COUNT];

bool firmware_update_is_slot(unsigned int id)
{
    return id == 0 || (id < CAN_NODE_SLOT_COUNT && (CAN_NODE_ID_MASK & (1U << id)));
}

/**
 * @brief Get the ELF SHA-256 of the running master image.
 */
static const uint8_t *master_hash(void)
{
    return esp_app_get_description()->app_elf_sha256;
}

/**
 * @brief Check whether a node's last identity says its image is on trial.
 *
 * @param id Node ID.
 * @return true if NODE_IDENTITY_PENDING was set.
 */
static bool node_on_trial(unsigned int id)
{
    return (nodes[id].frame.flags & NODE_IDENTITY_PENDING) != 0;
}

/**
 * @brief Check whether a slot has an image that differs from what it runs.
 *
 * @param id Slot ID.
 * @param hash Hash the slot runs, or NULL if unknown.
 */
static bool needs_update(unsigned int id, const uint8_t *hash)
{
    return images[id].present &&
           (!hash || memcmp(hash, images[id].hash, NODE_IDENTITY_HASH_LENGTH) != 0);
}

/**
 * @brief Write an identity hash as lowercase hex.
 *
 * @param hash NODE_IDENTITY_HASH_LENGTH bytes.
 * @param text At least 2 * NODE_IDENTITY_HASH_LENGTH + 1 bytes.
 */
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
    if (!nodes[id].heard_us || memcmp(&nodes[id].frame, &frame, sizeof(frame)) != 0) {
        char hash[2 * NODE_IDENTITY_HASH_LENGTH + 1];
        format_hash(frame.elf_sha256, hash);
        ESP_LOGI(TAG, "Node %u firmware %s protocol %04x%s", id, hash,
                 frame.protocol_hash,
                 (frame.flags & NODE_IDENTITY_PENDING) ? " (on trial)" : "");
        if (frame.protocol_hash != CAN_PROTOCOL_HASH) {
            ESP_LOGE(TAG, "Node %u protocol %04x, master %04x", id,
                     frame.protocol_hash, CAN_PROTOCOL_HASH);
        }
        if (needs_update(id, frame.elf_sha256)) {
            ESP_LOGW(TAG, "Node %u needs update: %s differs", id, images[id].path);
        }
    }
    nodes[id].frame = frame;
    nodes[id].heard_us = esp_timer_get_time();
}

bool firmware_update_node_protocol_differs(void)
{
    for (unsigned int id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if ((MASTER_EXPECTED_NODE_MASK & (1U << id)) && nodes[id].heard_us &&
            nodes[id].frame.protocol_hash != CAN_PROTOCOL_HASH) {
            return true;
        }
    }
    return false;
}

esp_err_t firmware_update_request_identity(void)
{
    uint8_t payload[] = {0xFF, UPDATE_OP_IDENTIFY};
    return can_master_send(CAN_ID_UPDATE_COMMAND, payload, sizeof(payload));
}

bool firmware_update_on_trial(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void firmware_update_print_identity(void)
{
    firmware_update_status_t status;
    firmware_update_get_status(&status);
    for (unsigned int id = 0; id < (MASTER_CAN_ENABLED ? CAN_NODE_SLOT_COUNT : 1); id++) {
        if (!firmware_update_is_slot(id)) {
            continue;
        }
        const firmware_slot_status_t *slot = &status.slots[id];
        ESP_LOGI(TAG, "%s %u: %s%s%s", id == 0 ? "Master" : "Node", id,
                 slot->known ? slot->hash : "unknown", slot->on_trial ? " (on trial)" : "",
                 slot->needs_update ? " (needs update)" : "");
    }
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

/* Own copies: tests/test_firmware_update.py compiles parse_image() without ESP-IDF. */
#define IMAGE_MAGIC               0xE9
#define IMAGE_APP_DESC_OFFSET     32
#define APP_DESC_MAGIC            0xABCD5432
#define APP_DESC_ELF_SHA256_OFFSET 144

/**
 * @brief Read the identity and hash from the start of an image.
 *
 * @param header Start of the image file.
 * @param length Bytes in header.
 * @param identity Set to the firmware_identity_t after esp_app_desc_t.
 * @param hash Set to the first NODE_IDENTITY_HASH_LENGTH bytes of the ELF
 *             SHA-256.
 * @return false if header is too short to hold the identity, or the image,
 *         app descriptor or identity magic does not match.
 */
static bool parse_image(const uint8_t *header, size_t length, firmware_identity_t *identity,
                        uint8_t *hash)
{
    if (length < FIRMWARE_IDENTITY_OFFSET + sizeof(*identity) || header[0] != IMAGE_MAGIC) {
        return false;
    }
    const uint8_t *desc = header + IMAGE_APP_DESC_OFFSET;
    uint32_t desc_magic;
    memcpy(&desc_magic, desc, sizeof(desc_magic));
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
_Static_assert(ESP_IMAGE_HEADER_MAGIC == IMAGE_MAGIC, "image magic must match esp_app_format.h");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == APP_DESC_MAGIC, "app desc magic must match esp_app_desc.h");
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) ==
                   IMAGE_APP_DESC_OFFSET,
               "app desc must follow the first segment header");

/**
 * @brief Read the identity, hash and size of an image file.
 *
 * See parse_image().
 *
 * @param size Set to the file size.
 * @return false if the file cannot be read or is not a firmware image.
 */
static bool read_image(const char *path, firmware_identity_t *identity, uint8_t *hash,
                       uint32_t *size)
{
    uint8_t header[FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t)];
    struct stat info;
    FILE *file = fopen(path, "rb");
    bool read_ok = file && fstat(fileno(file), &info) == 0 &&
                   fread(header, 1, sizeof(header), file) == sizeof(header);
    if (file) {
        fclose(file);
    }
    if (!read_ok || !parse_image(header, sizeof(header), identity, hash)) {
        return false;
    }
    *size = (uint32_t)info.st_size;
    return true;
}

/**
 * @brief Tell why an image cannot be stored.
 *
 * @return "bench-only build" or "unknown node", or NULL if it can be stored.
 */
static const char *reject_reason(const firmware_identity_t *identity)
{
    if (identity->bench_only) {
        return "bench-only build";
    }
    if (!firmware_update_is_slot(identity->node_id)) {
        return "unknown node";
    }
    return NULL;
}

/**
 * @brief Build the stored path of a slot's image: master.bin or node<N>.bin.
 */
static void image_path(unsigned int node_id, char *path, size_t size)
{
    if (node_id == 0) {
        snprintf(path, size, FIRMWARE_DIR "/master.bin");
    } else {
        snprintf(path, size, FIRMWARE_DIR "/node%u.bin", node_id);
    }
}

/**
 * @brief Read a slot's image from image_path() into images.
 *
 * The slot is left without an image if the file is missing, is not a
 * firmware image, is for another slot, or reject_reason() refuses it.
 *
 * @param id Slot accepted by firmware_update_is_slot().
 */
static void load_image(unsigned int id)
{
    image_t *image = &images[id];
    firmware_identity_t identity;
    memset(image, 0, sizeof(*image));
    image_path(id, image->path, sizeof(image->path));
    if (read_image(image->path, &identity, image->hash, &image->size) &&
        identity.node_id == id && !reject_reason(&identity)) {
        image->present = true;
        image->protocol_hash = identity.protocol_hash;
    }
}

/**
 * @brief Run load_image() for every slot.
 */
static void load_images(void)
{
    for (unsigned int id = 0; id < CAN_NODE_SLOT_COUNT; id++) {
        if (firmware_update_is_slot(id)) {
            load_image(id);
        }
    }
}

esp_err_t firmware_update_store_image(const char *temp_path, const char **stored_name,
                                      const char **reason)
{
    firmware_identity_t identity;
    uint8_t hash[NODE_IDENTITY_HASH_LENGTH];
    uint32_t size;
    if (!read_image(temp_path, &identity, hash, &size)) {
        *reason = "not a firmware image";
    } else {
        *reason = reject_reason(&identity);
    }
    if (*reason) {
        unlink(temp_path);
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = images[identity.node_id].path;
    /* FAT cannot rename onto an existing file. */
    unlink(path);
    bool stored = (rename(temp_path, path) == 0);
    if (!stored) {
        ESP_LOGE(TAG, "Cannot store %s as %s", temp_path, path);
        unlink(temp_path);
    }
    load_image(identity.node_id);
    if (!stored) {
        return ESP_FAIL;
    }
    *stored_name = strrchr(path, '/') + 1;
    return ESP_OK;
}

esp_err_t firmware_update_delete_image(unsigned int node_id)
{
    esp_err_t result = unlink(images[node_id].path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    load_image(node_id);
    return result;
}

void firmware_update_get_status(firmware_update_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->running = running;
    status->target = target;
    status->percent = percent;
    for (unsigned int id = 0; id < CAN_NODE_SLOT_COUNT; id++) {
        firmware_slot_status_t *slot = &status->slots[id];
        const uint8_t *hash = id == 0 ? master_hash() : nodes[id].frame.elf_sha256;
        slot->known = id == 0 || nodes[id].heard_us != 0;
        if (slot->known) {
            format_hash(hash, slot->hash);
            slot->on_trial = id == 0 ? firmware_update_on_trial() : node_on_trial(id);
            slot->wrong_protocol = (id != 0 && nodes[id].frame.protocol_hash != CAN_PROTOCOL_HASH);
            slot->needs_update = needs_update(id, hash);
        }
        slot->image = images[id].present;
        slot->result = results[id];
    }
}

#if MASTER_CAN_ENABLED
/**
 * @brief Tell a node to drop the update it is receiving.
 *
 * Send errors are ignored.
 *
 * @param id Node ID.
 */
static void send_abort(unsigned int id)
{
    uint8_t payload[] = {(uint8_t)id, UPDATE_OP_ABORT};
    (void)can_master_send(CAN_ID_UPDATE_COMMAND, payload, sizeof(payload));
}

/**
 * @brief Send one chunk of an image as CAN_ID_UPDATE_DATA frames.
 *
 * Send errors are ignored; the node asks for the chunk again.
 *
 * @param file Image file.
 * @param offset Start of the chunk in the image.
 * @param length Chunk length, at most UPDATE_CHUNK_BYTES.
 * @param tag Tag from the node's READ, sent in every frame.
 * @return false if the chunk could not be read from the file.
 */
static bool send_chunk(FILE *file, uint32_t offset, uint32_t length, uint8_t tag)
{
    uint8_t chunk[UPDATE_CHUNK_BYTES];
    if (fseek(file, offset, SEEK_SET) != 0 || fread(chunk, 1, length, file) != length) {
        return false;
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
    return true;
}

static const char *const error_names[UPDATE_ERROR_COUNT] = {
    [UPDATE_ERROR_NONE] = "failed: unknown error",
    [UPDATE_ERROR_RECORDING] = "failed: recording",
    [UPDATE_ERROR_TOO_LARGE] = "failed: too large",
    [UPDATE_ERROR_ON_TRIAL] = "failed: on trial",
    [UPDATE_ERROR_WRONG_IMAGE] = "failed: wrong image",
    [UPDATE_ERROR_FLASH] = "failed: flash",
    [UPDATE_ERROR_BAD_IMAGE] = "failed: bad image",
    [UPDATE_ERROR_TIMEOUT] = "failed: timeout",
};

/**
 * @brief Send an image to a node, answering each chunk it asks for.
 *
 * Sends UPDATE_OP_BEGIN, then serves READ replies until DONE, ERROR, or
 * UPDATE_REPLY_TIMEOUT_MS without a reply. A timeout after the last chunk
 * counts as done. Updates percent.
 *
 * @param file Open image file.
 * @param id Node ID.
 * @param image Stored image of the node.
 * @return NULL once the node has all the data, otherwise the result to show.
 */
static const char *transfer_image(FILE *file, unsigned int id, const image_t *image)
{
    xQueueReset(reply_queue);
    uint8_t begin[6] = {(uint8_t)id, UPDATE_OP_BEGIN};
    memcpy(begin + 2, &image->size, sizeof(image->size));
    if (can_master_send(CAN_ID_UPDATE_COMMAND, begin, sizeof(begin)) != ESP_OK) {
        return "failed: CAN send";
    }
    bool last_chunk_sent = false;
    while (true) {
        can_message_t reply;
        if (xQueueReceive(reply_queue, &reply, pdMS_TO_TICKS(UPDATE_REPLY_TIMEOUT_MS)) != pdTRUE) {
            if (last_chunk_sent) {
                /* DONE was lost; the confirmation wait shows whether the node rebooted. */
                return NULL;
            }
            send_abort(id);
            return "failed: no reply";
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
            return NULL;
        }
        if (kind == UPDATE_REPLY_ERROR && reply.dlc >= 2) {
            uint8_t code = (uint8_t)(reply.data >> 8);
            return error_names[code < UPDATE_ERROR_COUNT ? code : UPDATE_ERROR_NONE];
        }
        if (kind != UPDATE_REPLY_READ || reply.dlc < 6) {
            continue;
        }
        uint32_t offset = (uint32_t)(reply.data >> 8);
        uint8_t tag = (uint8_t)(reply.data >> 40);
        if (offset >= image->size) {
            send_abort(id);
            return "failed: bad offset";
        }
        uint32_t length = image->size - offset;
        if (length > UPDATE_CHUNK_BYTES) {
            length = UPDATE_CHUNK_BYTES;
        }
        if (!send_chunk(file, offset, length, tag)) {
            send_abort(id);
            return "failed: SD read";
        }
        last_chunk_sent = (offset + length == image->size);
        percent = (uint64_t)offset * 100 / image->size;
    }
}

/**
 * @brief Update one node and wait until it confirms the new image.
 *
 * After the transfer, asks for identities every IDENTITY_REQUEST_MS until the node reports
 * the new hash off trial, for up to UPDATE_CONFIRM_WAIT_MS.
 *
 * @return "updated", or a result starting with "failed:".
 */
static const char *flash_node(unsigned int id, const image_t *image)
{
    FILE *file = fopen(image->path, "rb");
    if (!file) {
        return "failed: cannot open image";
    }
    const char *failure = transfer_image(file, id, image);
    fclose(file);
    if (failure) {
        return failure;
    }
    for (int waited = 0; waited < UPDATE_CONFIRM_WAIT_MS; waited += IDENTITY_POLL_MS) {
        if (waited % IDENTITY_REQUEST_MS == 0) {
            (void)firmware_update_request_identity();
        }
        vTaskDelay(pdMS_TO_TICKS(IDENTITY_POLL_MS));
        if (memcmp(nodes[id].frame.elf_sha256, image->hash, NODE_IDENTITY_HASH_LENGTH) == 0 &&
            !node_on_trial(id)) {
            return "updated";
        }
    }
    return "failed: new image not confirmed";
}

/**
 * @brief Flash every node whose stored image differs, one at a time.
 *
 * Nodes that do not answer an identity request within IDENTITY_WAIT_MS are
 * skipped, and so are images for another protocol than the master's.
 */
static void update_nodes(void)
{
    int64_t asked_us = esp_timer_get_time();
    (void)firmware_update_request_identity();
    vTaskDelay(pdMS_TO_TICKS(IDENTITY_WAIT_MS));
    for (unsigned int id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if (!firmware_update_is_slot(id)) {
            continue;
        }
        const uint8_t *hash = nodes[id].heard_us >= asked_us ? nodes[id].frame.elf_sha256 : NULL;
        const char *result;
        if (!images[id].present) {
            result = "no image";
        } else if (!hash) {
            result = "offline";
        } else if (!needs_update(id, hash)) {
            result = "current";
        } else if (images[id].protocol_hash != CAN_PROTOCOL_HASH) {
            /* Left over when a new master image rolled back. */
            result = "skipped: image for another protocol";
        } else {
            ESP_LOGI(TAG, "Node %u: flashing %s", id, images[id].path);
            percent = 0;
            target = (int)id;
            result = flash_node(id, &images[id]);
        }
        results[id] = result;
        ESP_LOGI(TAG, "Node %u: %s", id, result);
    }
}
#endif

/**
 * @brief Flash the stored master image to the next OTA slot and boot it next.
 *
 * Does not restart.
 *
 * @return NULL once the new image is set to boot, otherwise the result to show.
 */
static const char *update_master(void)
{
    const image_t *image = &images[0];
    if (!image->present) {
        return "no image";
    }
    if (!needs_update(0, master_hash())) {
        return "current";
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    /* esp_ota_begin leaks its handle entry when the erase fails. */
    if (!part || image->size > part->size) {
        return "failed: image too large";
    }
    esp_ota_handle_t handle;
    esp_err_t error = esp_ota_begin(part, image->size, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? "failed: running image not confirmed"
                                                           : "failed: flash";
    }
    FILE *file = fopen(image->path, "rb");
    if (!file) {
        (void)esp_ota_abort(handle);
        return "failed: cannot open image";
    }
    ESP_LOGI(TAG, "Master: flashing %s", image->path);
    static uint8_t buffer[MASTER_WRITE_BYTES];
    uint32_t offset = 0;
    while (offset < image->size) {
        size_t length = image->size - offset;
        if (length > sizeof(buffer)) {
            length = sizeof(buffer);
        }
        const char *failure = NULL;
        if (fread(buffer, 1, length, file) != length) {
            failure = "failed: SD read";
        } else if (esp_ota_write(handle, buffer, length) != ESP_OK) {
            failure = "failed: flash";
        }
        if (failure) {
            (void)esp_ota_abort(handle);
            fclose(file);
            return failure;
        }
        offset += length;
        percent = (uint64_t)offset * 100 / image->size;
    }
    fclose(file);
    error = esp_ota_end(handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_OTA_VALIDATE_FAILED ? "failed: bad image" : "failed: flash";
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return "failed: flash";
    }
    return NULL;
}

/**
 * @brief Leave a resume marker and restart into the image set to boot.
 *
 * Does not return.
 */
static void restart_into_new_image(void)
{
    results[0] = "updated; restarting";
    ESP_LOGI(TAG, "Master: %s", results[0]);
    FILE *marker = fopen(RESUME_PATH, "w");
    if (!marker || fclose(marker) != 0) {
        ESP_LOGE(TAG, "Cannot write %s; press Update after the restart", RESUME_PATH);
    }
    vTaskDelay(pdMS_TO_TICKS(MASTER_RESTART_DELAY_MS));
    esp_restart();
}

/**
 * @brief Timer callback that restarts to roll back an image still on trial.
 */
static void confirm_timeout(void *argument)
{
    (void)argument;
    if (firmware_update_on_trial()) {
        ESP_LOGE(TAG, "New image not confirmed; restarting to roll back");
        esp_restart();
    }
}

esp_err_t firmware_update_init(void)
{
    reply_queue = xQueueCreate(16, sizeof(can_message_t));
    if (!reply_queue) {
        return ESP_ERR_NO_MEM;
    }
    /* An image that hangs before the end of app_main would otherwise stay on trial forever. */
    if (firmware_update_on_trial()) {
        static esp_timer_handle_t timer;
        const esp_timer_create_args_t args = {.callback = confirm_timeout, .name = "fw_confirm"};
        esp_err_t error = esp_timer_create(&args, &timer);
        if (error != ESP_OK) {
            return error;
        }
        error = esp_timer_start_once(timer, (uint64_t)MASTER_CONFIRM_TIMEOUT_MS * 1000);
        if (error != ESP_OK) {
            return error;
        }
    }
    load_images();
    if (needs_update(0, master_hash())) {
        ESP_LOGW(TAG, "Master needs update: %s differs", images[0].path);
    }
    if (access(RESUME_PATH, F_OK) == 0) {
        resume = images[0].present && !needs_update(0, master_hash());
        if (!resume) {
            unlink(RESUME_PATH);
            results[0] = "failed: new image rolled back";
            ESP_LOGE(TAG, "Master: %s", results[0]);
        }
    }
    return ESP_OK;
}

bool firmware_update_resume_pending(void)
{
    return resume;
}

void firmware_update_run(void)
{
    target = -1;
    percent = 0;
    memset(results, 0, sizeof(results));
    running = true;
    load_images();

    if (firmware_update_on_trial()) {
        /* The marker stays: the rollback restart reports it. */
        results[0] = "failed: new image not confirmed";
    } else if (resume) {
        unlink(RESUME_PATH);
        results[0] = "updated";
    } else {
        target = 0;
        results[0] = update_master();
        if (!results[0]) {
            restart_into_new_image();
        }
    }
    resume = false;
    ESP_LOGI(TAG, "Master: %s", results[0]);
#if MASTER_CAN_ENABLED
    if (strncmp(results[0], "failed", 6) != 0) {
        update_nodes();
    }
#endif
    target = -1;
    running = false;
}

void firmware_update_confirm(void)
{
    if (!firmware_update_on_trial()) {
        return;
    }
    esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "New image confirmed");
    } else {
        ESP_LOGW(TAG, "Cannot confirm new image: %s", esp_err_to_name(error));
    }
}
