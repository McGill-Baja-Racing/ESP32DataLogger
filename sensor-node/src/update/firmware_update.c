#include "firmware_update.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "can/can_node.h"
#include "protocol/app_protocol.h"
#include "sdkconfig.h"

#ifndef CAN_PROTOCOL_HASH
#error "CAN_PROTOCOL_HASH comes from protocol/build_hooks.py"
#endif

/* sdkconfig.defaults only fills options a stale sdkconfig.<env> lacks. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Rollback is off: delete sdkconfig.<env> and rebuild"
#endif

#ifndef SENSOR_SERIAL_TEST
#define SENSOR_SERIAL_TEST 0
#endif

#define UPDATE_QUEUE_LENGTH       (UPDATE_CHUNK_FRAMES + 8)
#define UPDATE_RETRY_MS           100
#define UPDATE_RETRY_LIMIT        20
#define UPDATE_CONFIRM_TIMEOUT_MS 30000

static const char *TAG = "FirmwareUpdate";

typedef enum {
    INPUT_COMMAND,
    INPUT_DATA,
} input_kind_t;

typedef struct {
    input_kind_t kind;
    uint8_t length;
    uint8_t bytes[8];
} update_input_t;

static QueueHandle_t input_queue;
static bool receiving;
static esp_ota_handle_t ota_handle;
static const esp_partition_t *update_partition;
static uint32_t image_size;
static uint32_t offset;
static uint8_t tag;
static uint8_t chunk[UPDATE_CHUNK_BYTES];
static uint32_t received;
static uint32_t chunk_length;
static uint32_t retries;
static TickType_t requested_at;

const __attribute__((section(".rodata_custom_desc"))) firmware_identity_t firmware_identity = {
    .magic = FIRMWARE_IDENTITY_MAGIC,
    .node_id = NODE_ID,
    .bench_only = SENSOR_SERIAL_TEST,
    .protocol_hash = CAN_PROTOCOL_HASH,
};

_Static_assert(FIRMWARE_IDENTITY_OFFSET / UPDATE_CHUNK_BYTES ==
               (FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t) - 1) / UPDATE_CHUNK_BYTES,
               "accept_chunk needs the identity inside one chunk");

/**
 * @brief Check whether the running image is on trial.
 *
 * @return true while the running partition is ESP_OTA_IMG_PENDING_VERIFY.
 */
static bool is_on_trial(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void firmware_update_report_identity(void)
{
    node_identity_frame_t frame = {
        .protocol_hash = CAN_PROTOCOL_HASH,
    };
    memcpy(frame.elf_sha256, esp_app_get_description()->app_elf_sha256,
           NODE_IDENTITY_HASH_LENGTH);
    if (is_on_trial()) {
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
    if (length < 2) {
        return;
    }
    if (payload[1] == UPDATE_OP_IDENTIFY) {
        firmware_update_report_identity();
        return;
    }
    if ((payload[1] == UPDATE_OP_BEGIN && length >= 6 && payload[0] == NODE_ID) ||
        payload[1] == UPDATE_OP_ABORT) {
        update_input_t input = {.kind = INPUT_COMMAND, .length = length};
        memcpy(input.bytes, payload, length);
        (void)xQueueSend(input_queue, &input, 0);
    }
}

void firmware_update_handle_data(const uint8_t *payload, uint8_t length)
{
    update_input_t input = {.kind = INPUT_DATA, .length = length};
    memcpy(input.bytes, payload, length);
    (void)xQueueSend(input_queue, &input, 0);
}

/**
 * @brief Send an update reply on CAN_ID_NODE_UPDATE_BASE + NODE_ID.
 *
 * A failed send is logged and not retried.
 */
static void send_reply(const uint8_t *payload, uint8_t length)
{
    esp_err_t error = can_node_send(CAN_ID_NODE_UPDATE_BASE + NODE_ID, payload, length);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Update reply failed: %s", esp_err_to_name(error));
    }
}

/**
 * @brief Drop the update being received, if any.
 */
static void stop_receiving(void)
{
    if (receiving) {
        esp_ota_abort(ota_handle);
        receiving = false;
    }
}

/**
 * @brief Drop the update being received and report the error to the master.
 *
 * @param error Sent in an UPDATE_REPLY_ERROR and logged.
 */
static void fail(update_error_t error)
{
    stop_receiving();
    uint8_t reply[] = {UPDATE_REPLY_ERROR, error};
    send_reply(reply, sizeof(reply));
    ESP_LOGW(TAG, "Update failed: error %d", error);
}

/**
 * @brief Ask the master for the chunk at offset, under the next tag.
 *
 * The 3-bit tag changes with every request, so frames still arriving for an
 * earlier one are ignored. Restarts the retry timer.
 */
static void request_chunk(void)
{
    tag = (tag + 1) & 7;
    chunk_length = image_size - offset;
    if (chunk_length > UPDATE_CHUNK_BYTES) {
        chunk_length = UPDATE_CHUNK_BYTES;
    }
    received = 0;
    uint8_t reply[] = {
        UPDATE_REPLY_READ,
        (uint8_t)offset, (uint8_t)(offset >> 8), (uint8_t)(offset >> 16), (uint8_t)(offset >> 24),
        tag,
    };
    send_reply(reply, sizeof(reply));
    requested_at = xTaskGetTickCount();
}

/**
 * @brief Start receiving an image.
 *
 * Drops any update in progress. Fails with UPDATE_ERROR_RECORDING while the
 * node is active, UPDATE_ERROR_BAD_IMAGE if the image is too small to hold
 * firmware_identity_t, UPDATE_ERROR_TOO_LARGE if it does not fit the next OTA
 * partition, UPDATE_ERROR_ON_TRIAL if the running image is not confirmed, and
 * UPDATE_ERROR_FLASH if the partition cannot be prepared.
 *
 * @param bytes UPDATE_OP_BEGIN command: target, op, u32 image size,
 *              little-endian.
 */
static void begin_update(const uint8_t *bytes)
{
    stop_receiving();
    if (can_node_state() == NODE_STATE_ACTIVE) {
        fail(UPDATE_ERROR_RECORDING);
        return;
    }
    uint32_t size = (uint32_t)bytes[2] | (uint32_t)bytes[3] << 8 |
                    (uint32_t)bytes[4] << 16 | (uint32_t)bytes[5] << 24;
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (size < FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t)) {
        fail(UPDATE_ERROR_BAD_IMAGE);
        return;
    }
    if (!update_partition || size > update_partition->size) {
        fail(UPDATE_ERROR_TOO_LARGE);
        return;
    }
    esp_err_t error = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (error != ESP_OK) {
        fail(error == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? UPDATE_ERROR_ON_TRIAL
                                                         : UPDATE_ERROR_FLASH);
        return;
    }
    ESP_LOGI(TAG, "Update started, %" PRIu32 " bytes", size);
    receiving = true;
    image_size = size;
    offset = 0;
    retries = 0;
    request_chunk();
}

/**
 * @brief Check the received image, boot it next, reply DONE and restart.
 *
 * Returns only after a failure, once the error is sent.
 */
static void finish_update(void)
{
    receiving = false;
    esp_err_t error = esp_ota_end(ota_handle);
    if (error != ESP_OK) {
        fail(error == ESP_ERR_OTA_VALIDATE_FAILED ? UPDATE_ERROR_BAD_IMAGE : UPDATE_ERROR_FLASH);
        return;
    }
    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        fail(UPDATE_ERROR_FLASH);
        return;
    }
    uint8_t reply[] = {UPDATE_REPLY_DONE};
    send_reply(reply, sizeof(reply));
    ESP_LOGI(TAG, "Update written; restarting");
    esp_restart();
}

/**
 * @brief Write a complete chunk, then ask for the next one or finish.
 *
 * The chunk holding firmware_identity_t fails with UPDATE_ERROR_WRONG_IMAGE
 * unless the image is for this NODE_ID and not bench-only.
 */
static void accept_chunk(void)
{
    if (offset <= FIRMWARE_IDENTITY_OFFSET &&
        FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t) <= offset + chunk_length) {
        firmware_identity_t identity;
        memcpy(&identity, chunk + (FIRMWARE_IDENTITY_OFFSET - offset), sizeof(identity));
        if (identity.magic != FIRMWARE_IDENTITY_MAGIC || identity.node_id != NODE_ID ||
            identity.bench_only != 0) {
            fail(UPDATE_ERROR_WRONG_IMAGE);
            return;
        }
    }
    if (esp_ota_write(ota_handle, chunk, chunk_length) != ESP_OK) {
        fail(UPDATE_ERROR_FLASH);
        return;
    }
    offset += chunk_length;
    retries = 0;
    if (offset == image_size) {
        finish_update();
    } else {
        request_chunk();
    }
}

/**
 * @brief Handle one input on the update task.
 *
 * @param input Queued command or data frame.
 */
static void handle_input(const update_input_t *input)
{
    if (input->kind == INPUT_COMMAND) {
        if (input->bytes[1] == UPDATE_OP_BEGIN) {
            begin_update(input->bytes);
        } else {
            stop_receiving();
        }
        return;
    }
    uint8_t index = input->bytes[0] & UPDATE_INDEX_MASK;
    uint32_t frames = (chunk_length + UPDATE_FRAME_BYTES - 1) / UPDATE_FRAME_BYTES;
    if (!receiving || (input->bytes[0] >> UPDATE_TAG_SHIFT) != tag || index >= frames) {
        return;
    }
    uint32_t start = (uint32_t)UPDATE_FRAME_BYTES * index;
    uint32_t bytes = chunk_length - start;
    if (bytes > UPDATE_FRAME_BYTES) {
        bytes = UPDATE_FRAME_BYTES;
    }
    if (input->length != 1 + bytes) {
        return;
    }
    memcpy(chunk + start, input->bytes + 1, bytes);
    received |= UINT32_C(1) << index;
    uint32_t all = frames == 32 ? UINT32_MAX : (UINT32_C(1) << frames) - 1;
    if (received == all) {
        accept_chunk();
    }
}

/**
 * @brief Task that runs updates from input_queue.
 *
 * Asks again for the current chunk every UPDATE_RETRY_MS without it, and
 * fails with UPDATE_ERROR_TIMEOUT after UPDATE_RETRY_LIMIT retries in a row.
 * Restarts to roll back an image still on trial UPDATE_CONFIRM_TIMEOUT_MS
 * after boot.
 */
static void update_task(void *argument)
{
    (void)argument;
    update_input_t input;
    while (true) {
        if (xQueueReceive(input_queue, &input, pdMS_TO_TICKS(UPDATE_RETRY_MS)) == pdTRUE) {
            handle_input(&input);
        }
        if (receiving && xTaskGetTickCount() - requested_at >= pdMS_TO_TICKS(UPDATE_RETRY_MS)) {
            if (++retries > UPDATE_RETRY_LIMIT) {
                fail(UPDATE_ERROR_TIMEOUT);
            } else {
                request_chunk();
            }
        }
        /* A new image that cannot reach the master must not stay installed. */
        if (is_on_trial() &&
            xTaskGetTickCount() >= pdMS_TO_TICKS(UPDATE_CONFIRM_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "New image not confirmed; restarting to roll back");
            esp_restart();
        }
    }
}

esp_err_t firmware_update_init(void)
{
    input_queue = xQueueCreate(UPDATE_QUEUE_LENGTH, sizeof(update_input_t));
    if (!input_queue) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(update_task, "fw_update", 6144, NULL, 5, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}

void firmware_update_confirm(void)
{
    if (!is_on_trial()) {
        return;
    }
    esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Confirm failed: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "New image confirmed");
    firmware_update_report_identity();
}
