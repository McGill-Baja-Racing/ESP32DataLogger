#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Start the task that receives updates and follows the master.
 *
 * Call before can_node_init(): its update callbacks feed this task. If the
 * running image is on trial, the task restarts the node 30 s after boot to
 * roll back, unless firmware_update_confirm() runs first.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The queue or the task could not be created.
 */
esp_err_t firmware_update_init(void);

/**
 * @brief Send this node's identity to the master.
 *
 * Sends node_identity_frame_t on CAN_ID_NODE_IDENTITY_BASE + NODE_ID, with
 * NODE_IDENTITY_PENDING while the image is on trial. A failed send is logged
 * and not retried.
 */
void firmware_update_report_identity(void);

/**
 * @brief Handle an update command for this node.
 *
 * Answers UPDATE_OP_IDENTIFY at once. Queues UPDATE_OP_BEGIN, if it carries
 * the image size, and UPDATE_OP_ABORT for the update task; a full queue drops
 * them. Ignores other commands.
 *
 * @param payload Target node, update_op_t, then the op's data.
 * @param length Payload bytes, 0-8.
 */
void firmware_update_handle_command(const uint8_t *payload, uint8_t length);

/**
 * @brief Queue an update data frame for the update task.
 *
 * A full queue drops it; the node asks for the chunk again.
 *
 * @param payload CAN_ID_UPDATE_DATA payload.
 * @param length Payload bytes, 0-8.
 */
void firmware_update_handle_data(const uint8_t *payload, uint8_t length);

/**
 * @brief Queue a master identity announcement for the update task.
 *
 * See follow_master() in firmware_update.c. Ignored unless it is a whole
 * node_identity_frame_t; a full queue drops it.
 *
 * @param payload CAN_ID_MASTER_IDENTITY payload.
 * @param length Payload bytes.
 */
void firmware_update_handle_master(const uint8_t *payload, uint8_t length);

/**
 * @brief Check whether a master identity announcement has arrived.
 *
 * @return true once the update task has handled one.
 */
bool firmware_update_master_heard(void);

/**
 * @brief Mark the running image as good, then report the new identity.
 *
 * Does nothing unless the image is on trial. A failure is logged and the
 * image stays on trial.
 */
void firmware_update_confirm(void);
