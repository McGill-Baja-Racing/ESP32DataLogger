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
    bool wrong_protocol;
    bool needs_update;
    bool image;
    const char *result;
} firmware_slot_status_t;

typedef struct {
    int target;
    unsigned int percent;
    firmware_slot_status_t slots[CAN_NODE_SLOT_COUNT];
} firmware_update_status_t;

/**
 * @brief Check whether a frame is a node identity frame.
 *
 * @param message Received frame.
 * @return true for CAN_ID_NODE_IDENTITY_BASE + node ID, nodes 1 and up.
 */
bool firmware_update_is_identity_frame(const can_message_t *message);

/**
 * @brief Store a node's identity and log it if it changed.
 *
 * Also logs an error if the node runs another protocol than the master, and
 * a warning if its stored image differs from what it runs. Ignores frames
 * that are not 8 bytes.
 *
 * @param message Frame accepted by firmware_update_is_identity_frame().
 */
void firmware_update_handle_identity(const can_message_t *message);

/**
 * @brief Ask every node to send its identity.
 *
 * @return The can_master_send() result.
 */
esp_err_t firmware_update_request_identity(void);

/**
 * @brief Start the task that announces the master's identity to the nodes.
 *
 * Every 500 ms it sends CAN_ID_MASTER_IDENTITY, never flagged as on trial.
 * Send errors are ignored. CAN builds only.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The task could not be created.
 */
esp_err_t firmware_update_start_announcing(void);

/**
 * @brief Log the firmware hash of the master and each node.
 *
 * For the serial status command. Marks images on trial and slots whose stored
 * image differs. Lists nodes in CAN builds only.
 */
void firmware_update_print_identity(void);

/**
 * @brief Restore interrupted image stores and scan FIRMWARE_DIR.
 *
 * Needs the SD card mounted. If the running image is on trial, starts a timer
 * that restarts the master after 60 s to roll back, unless
 * firmware_update_confirm() runs first.
 *
 * @retval ESP_OK          Ready.
 * @retval ESP_ERR_NO_MEM  The reply queue or the files lock could not be
 *                         created.
 * @return Other esp_timer errors.
 */
esp_err_t firmware_update_init(void);

/**
 * @brief Check whether a frame is a node's update reply.
 *
 * @param message Received frame.
 * @return true for CAN_ID_NODE_UPDATE_BASE + node ID, nodes 1 and up.
 */
bool firmware_update_is_reply_frame(const can_message_t *message);

/**
 * @brief Queue an update reply for firmware_update_run().
 *
 * Does not block; a reply that finds the queue (16) full is dropped.
 *
 * @param message Frame accepted by firmware_update_is_reply_frame().
 */
void firmware_update_handle_reply(const can_message_t *message);

/**
 * @brief Flash every node whose stored image differs, then the master.
 *
 * Blocks for the whole update, so run it on its own task. Gives up at once
 * if the files lock is taken. Nodes that do not answer an identity request
 * within 500 ms are skipped. A node image is skipped if its protocol differs
 * from the one the master will run after this update. If the master image
 * changes the protocol but some node would not end up on it, the master keeps
 * its image and no node changes protocol. A node flashed to another protocol
 * keeps booting its old slot until the master switches. Any node failure
 * skips the master. Without CAN, only the master is updated and a protocol
 * change is refused.
 *
 * Progress and results show in firmware_update_get_status(). After flashing
 * itself, the master restarts 2 s later and this does not return.
 */
void firmware_update_run(void);

/**
 * @brief Mark the running image as good so it is not rolled back.
 *
 * Does nothing unless the image is on trial. A failure is logged.
 */
void firmware_update_confirm(void);

/**
 * @brief Try to take the lock on the files in FIRMWARE_DIR, without waiting.
 *
 * firmware_update_store_image() and firmware_update_delete_image() need it
 * held by the caller; firmware_update_run() takes it itself.
 *
 * @return true if taken; release it with firmware_update_unlock_files().
 */
bool firmware_update_lock_files(void);

/**
 * @brief Release the lock taken by firmware_update_lock_files().
 */
void firmware_update_unlock_files(void);

/**
 * @brief Check whether an ID is a firmware slot.
 *
 * @param id Slot ID.
 * @return true for 0, the master, or a node in CAN_NODE_ID_MASK.
 */
bool firmware_update_is_slot(unsigned int id);

/**
 * @brief Check an uploaded image and store it as the image of its node.
 *
 * Stores it as master.bin or node<N>.bin in FIRMWARE_DIR and deletes the
 * node's other images. The old image is kept as a .old backup until the new
 * one is in place, and put back if that fails. Needs the files lock held.
 *
 * @param temp_path Uploaded file; always gone afterwards.
 * @param stored_name Set to the stored file name on success.
 * @param stored_name_size Size of stored_name.
 * @param reason Set to why the image was refused, or NULL.
 * @retval ESP_OK               Stored.
 * @retval ESP_ERR_INVALID_ARG  Not a firmware image, a bench-only build, or
 *                              for an unknown node; see reason.
 * @retval ESP_FAIL             The file could not be renamed; the old image
 *                              stays.
 */
esp_err_t firmware_update_store_image(const char *temp_path, char *stored_name,
                                      size_t stored_name_size, const char **reason);

/**
 * @brief Delete every stored image of a slot.
 *
 * Needs the files lock held.
 *
 * @param node_id Slot accepted by firmware_update_is_slot(); not checked.
 * @retval ESP_OK             Deleted.
 * @retval ESP_ERR_NOT_FOUND  The last scan found no image for this slot.
 */
esp_err_t firmware_update_delete_image(unsigned int node_id);

/**
 * @brief Fill a status snapshot for /api/firmware and the serial console.
 *
 * Does not take a lock, so fields may change while it reads them. target is
 * the slot being flashed, or -1, and percent its progress. A node's slot is
 * known once its identity has arrived; the master's always is. Results are
 * from the last firmware_update_run() and stay valid.
 *
 * @param status Snapshot to fill; must not be NULL.
 */
void firmware_update_get_status(firmware_update_status_t *status);
