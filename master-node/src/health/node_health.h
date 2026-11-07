#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_master.h"
#include "esp_err.h"
#include "protocol/app_protocol.h"
#include "protocol/health_counters.h"

/**
 * @brief Count this reset and start the task that reports the master as node 0.
 *
 * Every NODE_STATUS_PERIOD_MS the task logs status and diagnostics frames
 * and, while recording, an event frame for each newly counted problem.
 * Counts HEALTH_BOOT with the reset reason.
 *
 * @retval ESP_OK The task is running.
 * @retval ESP_ERR_NO_MEM The task could not be created.
 */
esp_err_t node_health_init(void);

/* Fits the longest status and panel lines: every fault bit, six sensors and a restart. */
#define NODE_HEALTH_TEXT_MAX 640

/**
 * @brief Check whether a frame is a node's status, event or diagnostics frame.
 *
 * @param message Received frame.
 * @return true for a health frame from node 1 or up; node 0 is the master and
 *         is never on the bus.
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
 * @brief Describe a health frame in one comma-free line, for the CSV.
 *
 * @param id CAN ID of the frame.
 * @param data Frame payload as logged.
 * @param node Set to the node name; untouched for other IDs.
 * @param text Filled with the description.
 * @param size Size of text; a longer description is cut.
 * @return The frame kind such as "node_status", or NULL if id is not a health
 *         frame.
 */
const char *node_health_describe(uint32_t id, uint64_t data, const char **node,
                                 char *text, size_t size);

/**
 * @brief Name a health level, such as "warning".
 *
 * @param level Health level.
 * @return The name.
 */
const char *node_health_name_level(node_health_t level);

/**
 * @brief Name a sensor state as the log does, such as "no_readings".
 *
 * A UI uses node_health_label_sensor_state() instead.
 *
 * @param state Sensor state.
 * @return The name.
 */
const char *node_health_name_sensor_state(sensor_state_t state);

/**
 * @brief Log a health frame like a sample.
 *
 * Stamps status and diagnostics frames with the master time. Counts
 * HEALTH_EVENT_LOST for node events missing from the sequence or refused by
 * the logger. A repeated node event is dropped.
 *
 * @param message Health frame from the bus or the master itself.
 * @return true if logged or a repeat; false if refused or too short.
 */
bool node_health_handle_frame(const can_message_t *message);

/**
 * @brief List the problems to show on the web page, one per line.
 *
 * Lists each node, the master included, that is not ok or has restarted since
 * power-on; restarts show until the node is power-cycled. An expected node
 * with no recent status shows as not responding.
 *
 * @param out Filled with newline-ended lines; empty if none.
 * @param size Size of out, above 0; lines that do not fit are left out.
 */
void node_health_list_problems(char *out, size_t size);

/* A node's latest health, for a UI. Names use underscores, such as "mpu_node_3". */
typedef struct {
    const char *name;
    bool fresh;             /* a status arrived in the last 3 s */
    node_health_t level;
    uint32_t faults;        /* HEALTH_BIT() of each fault */
    struct {
        const char *name;   /* signal name, such as "engine_rpm" */
        sensor_state_t state;
    } sensors[NODE_SENSOR_SLOTS];   /* in status slot order, lowest CAN ID first */
    size_t sensor_count;
    uint8_t resets;         /* since power-on */
    const char *last_reset; /* reason of the last reset or power-on, such as "brownout" */
} node_health_view_t;

/**
 * @brief Get a node's health from its latest status and diagnostics frames.
 *
 * Use node_health_label_fault() and node_health_label_sensor_state() for words.
 *
 * @param node Node ID; 0 is the master.
 * @param view Filled in; a node never heard from reads ok and not fresh.
 * @return false, leaving view untouched, if node is out of range.
 */
bool node_health_get(uint8_t node, node_health_view_t *view);

/**
 * @brief Describe a fault in plain words for a UI, such as "GPS has no fix".
 *
 * @param fault Fault code.
 * @return The label, or NULL from HEALTH_FAULT_COUNT up.
 */
const char *node_health_label_fault(health_code_t fault);

/**
 * @brief Describe a sensor state in plain words for a UI, such as "no readings".
 *
 * @param state Sensor state.
 * @return The label.
 */
const char *node_health_label_sensor_state(sensor_state_t state);
