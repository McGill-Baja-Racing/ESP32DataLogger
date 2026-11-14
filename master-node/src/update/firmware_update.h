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
    bool running;
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
 * @brief Log the firmware hash of the master and each node.
 *
 * For the serial status command. Marks images on trial and slots whose stored
 * image differs. Lists nodes in CAN builds only.
 */
void firmware_update_print_identity(void);

/**
 * @brief Read the stored images and whether an update is to be resumed.
 *
 * See firmware_update_resume_pending(). If FIRMWARE_DIR/resume is left by an
 * image that rolled back, deletes it and reports the master update as
 * failed. Needs the SD card mounted. If the running image is on trial,
 * starts a timer that restarts the master after 60 s to roll back, unless
 * firmware_update_confirm() runs first.
 *
 * @retval ESP_OK          Ready.
 * @retval ESP_ERR_NO_MEM  The reply queue could not be created.
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
 * @brief Flash the master if its stored image differs, otherwise every node
 *        whose stored image differs.
 *
 * Blocks for the whole update, so run it on its own task, and only while
 * holding app_control_claim_firmware(). After flashing itself, the master
 * writes FIRMWARE_DIR/resume and restarts 2 s later; this does not return.
 * On every boot with that marker, app_control_resume_firmware_update() runs
 * this again; if the image is still on trial it only reports it as not
 * confirmed, otherwise it updates the nodes. Nodes that do not answer an identity
 * request within 500 ms are skipped, and so are node images for another
 * protocol than the master's. A failed master update skips the nodes.
 * Without CAN, only the master is updated.
 *
 * Progress and results show in firmware_update_get_status().
 */
void firmware_update_run(void);

/**
 * @brief Check whether the running master image is on trial.
 *
 * @return true while the running partition is ESP_OTA_IMG_PENDING_VERIFY.
 */
bool firmware_update_on_trial(void);

/**
 * @brief Check whether the next firmware_update_run() resumes an update.
 *
 * @return true from firmware_update_init() until that run starts, if the
 *         last update restarted the master and this is the image it installed.
 */
bool firmware_update_resume_pending(void);

/**
 * @brief Check whether an expected node runs another protocol than the master.
 *
 * @return true if a node in MASTER_EXPECTED_NODE_MASK last reported another
 *         protocol_hash.
 */
bool firmware_update_node_protocol_differs(void);

/**
 * @brief Mark the running image as good so it is not rolled back.
 *
 * Does nothing unless the image is on trial. A failure is logged.
 */
void firmware_update_confirm(void);

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
 * Stores it as master.bin or node<N>.bin in FIRMWARE_DIR, replacing the
 * slot's image. Call only while holding app_control_claim_firmware().
 *
 * @param temp_path Uploaded file; always gone afterwards.
 * @param stored_name Set to the stored file name on success; valid until the
 *                    slot's image changes.
 * @param reason Set to why the image was refused, or NULL.
 * @retval ESP_OK               Stored.
 * @retval ESP_ERR_INVALID_ARG  Not a firmware image, a bench-only build, or
 *                              for an unknown node; see reason.
 * @retval ESP_FAIL             The file could not be renamed; the slot is
 *                              left without an image.
 */
esp_err_t firmware_update_store_image(const char *temp_path, const char **stored_name,
                                      const char **reason);

/**
 * @brief Delete the stored image of a slot.
 *
 * Call only while holding app_control_claim_firmware().
 *
 * @param node_id Slot accepted by firmware_update_is_slot(); not checked.
 * @retval ESP_OK             Deleted.
 * @retval ESP_ERR_NOT_FOUND  The slot has no image file.
 */
esp_err_t firmware_update_delete_image(unsigned int node_id);

/**
 * @brief Fill a status snapshot for /api/firmware and the serial console.
 *
 * Does not take a lock, so fields may change while it reads them. running is
 * true while firmware_update_run() runs. target is the slot being flashed, or
 * -1, and percent its progress. A node's slot is
 * known once its identity has arrived; the master's always is. Results are
 * from the last firmware_update_run() and stay valid.
 *
 * @param status Snapshot to fill; must not be NULL.
 */
void firmware_update_get_status(firmware_update_status_t *status);
