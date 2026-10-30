#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "can/can_master.h"
#include "esp_err.h"
#include "protocol/app_protocol.h"

#define FIRMWARE_DIR "/sdcard/firmware"

typedef struct {
    bool known;
    char hash[2 * NODE_IDENTITY_HASH_LENGTH + 1];
    bool on_trial;
    bool needs_update;
    bool image;
    const char *result;
} firmware_slot_status_t;

typedef struct {
    int target;
    unsigned int percent;
    firmware_slot_status_t slots[CAN_NODE_SLOT_COUNT];
} firmware_update_status_t;

bool firmware_update_is_identity_frame(const can_message_t *message);
void firmware_update_handle_identity(const can_message_t *message);
esp_err_t firmware_update_request_identity(void);
void firmware_update_print_identity(void);
esp_err_t firmware_update_init(void);
bool firmware_update_is_reply_frame(const can_message_t *message);
void firmware_update_handle_reply(const can_message_t *message);
void firmware_update_run(void);
void firmware_update_confirm(void);
/* store_image and delete_image need the files lock held by the caller. */
bool firmware_update_lock_files(void);
void firmware_update_unlock_files(void);
bool firmware_update_is_slot(unsigned int id);
esp_err_t firmware_update_store_image(const char *temp_path, char *stored_name,
                                      size_t stored_name_size, const char **reason);
esp_err_t firmware_update_delete_image(unsigned int node_id);
void firmware_update_get_status(firmware_update_status_t *status);
