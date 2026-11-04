#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"
#include "protocol/app_protocol.h"

/**
 * @brief Count this reset and start the task that reports the master as node 0.
 *
 * Every NODE_STATUS_PERIOD_MS the task prints queued health frames, then logs
 * the master's status and diagnostics frames and, while recording, an event
 * frame for each code and subject counted since the last report. Also counts
 * HEALTH_BOOT with the reset reason as subject.
 *
 * @retval ESP_OK          The task is running.
 * @retval ESP_ERR_NO_MEM  The print queue or the task could not be created.
 */
esp_err_t node_health_init(void);

#define NODE_HEALTH_TEXT_MAX 320

/**
 * @brief Check whether a frame is a node's status, event or diagnostics frame.
 *
 * @param message Received frame.
 * @return true for a health frame from nodes 1 and up; node 0 is the master
 *         itself and never comes from the bus.
 */
bool node_health_is_frame(const can_message_t *message);

/**
 * @brief Get the node that sent a health frame.
 *
 * @param message Frame accepted by node_health_is_frame().
 * @return Node ID.
 */
uint8_t node_health_node_id(const can_message_t *message);

/**
 * @brief Describe a health frame in one line, for the console and the CSV.
 *
 * Works for node 0 too. The text has no commas.
 *
 * @param id CAN ID of the frame.
 * @param data Frame payload, as stored in the log.
 * @param node Set to the node name; untouched for other IDs.
 * @param text Filled with the description.
 * @param size Size of text; a longer description is cut.
 * @return "node_status", "health_event" or "node_diagnostics", or NULL if id
 *         is not a health frame.
 */
const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size);

/**
 * @brief Queue a health frame for printing and log it like a sample.
 *
 * Status and diagnostics frames get the master time in ms in their upper 4
 * bytes. Counts HEALTH_EVENT_LOST for node events that were lost on the way,
 * from gaps in the event sequence, or refused by the logger. A repeated node
 * event is dropped.
 *
 * @param message Health frame, from the bus or from the master itself.
 * @return true if the logger accepted the frame or it was a repeat; false if
 *         it was refused or too short.
 */
bool node_health_handle_frame(const can_message_t *message);

/**
 * @brief Record amount occurrences of code.
 *
 * Safe from interrupts. Codes counted per subject, such as HEALTH_CAN_TX, keep a separate count for
 * each subject while counters last. Does nothing when amount is 0.
 *
 * @param code What happened.
 * @param amount How many times.
 * @param subject What it happened to, such as a CAN ID; GPS IDs are mapped to
 *                one byte by protocol_health_subject().
 * @param error Error that caused it, or ESP_OK.
 */
void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error);

/**
 * @brief Set or clear a fault that lasts, such as no GPS fix.
 *
 * Safe from interrupts. Each onset also counts once, so a fault shorter than one status period
 * still shows.
 *
 * @param fault Fault code, below HEALTH_FAULT_COUNT.
 * @param active true while the fault lasts.
 */
void node_health_set(health_code_t fault, bool active);

/**
 * @brief List the problems to show on the web page, one per line.
 *
 * Lists each node's status while it is not ok and the latest event of each
 * node, code and subject for the last minute. Includes the master as node 0.
 * Events arrive only while recording, so at idle only statuses show.
 *
 * @param out Filled with the lines, each ending in a newline; empty if none.
 * @param size Size of out, above 0; lines that do not fit are left out.
 */
void node_health_list_problems(char *out, size_t size);
