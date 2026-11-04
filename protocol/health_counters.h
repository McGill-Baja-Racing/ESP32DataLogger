#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol/app_protocol.h"

/* Health counting, shared by the master's and the nodes' node_health.c. */

/* Extra counters that codes counted per subject take for their other subjects. */
#ifndef HEALTH_SHARED_COUNTERS
#define HEALTH_SHARED_COUNTERS 32
#endif
#define HEALTH_COUNTER_COUNT (HEALTH_CODE_COUNT + HEALTH_SHARED_COUNTERS)

/* Counts are cumulative, so unsigned differences survive wrap-around. */
typedef struct {
    uint8_t code;
    uint32_t count;
    uint32_t subject;
    esp_err_t error;
    uint32_t last_ms;       /* local esp_timer time of the last occurrence */
} health_counter_t;

/* Copied whole under the lock so a status and its events agree. */
typedef struct {
    health_counter_t counters[HEALTH_COUNTER_COUNT];
    bool active[HEALTH_FAULT_COUNT];
} health_state_t;

/**
 * @brief Count this reset, and HEALTH_BOOT with the reset reason as subject.
 *
 * @param resets Set to the resets since power-on, stopping at 255.
 * @param reset_reason Set to the esp_reset_reason_t of this boot.
 */
void node_health_count_boot(uint8_t *resets, uint8_t *reset_reason);

/**
 * @brief Count a problem. Safe from interrupts.
 *
 * Codes such as HEALTH_CAN_TX keep a count per subject while counters last.
 *
 * @param code What happened.
 * @param amount How many times; 0 does nothing.
 * @param subject What it happened to, such as a CAN ID; GPS IDs are mapped to
 *                one byte.
 * @param error Error that caused it, or ESP_OK.
 */
void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error);

/**
 * @brief Set or clear a lasting fault, such as no GPS fix. Safe from
 * interrupts.
 *
 * Each onset also counts once, so a fault shorter than one status still shows.
 *
 * @param fault Fault code, below HEALTH_FAULT_COUNT.
 * @param active true while the fault lasts.
 */
void node_health_set(health_code_t fault, bool active);

/**
 * @brief Copy every counter and lasting fault at once.
 *
 * @param copy Filled with the counters.
 */
void node_health_copy_counts(health_state_t *copy);

/**
 * @brief Get the status fault bits for one period.
 *
 * @param now Counters copied this period.
 * @param previous Counters copied last period.
 * @return HEALTH_BIT() of each fault active now or counted since previous.
 */
uint32_t node_health_get_faults(const health_state_t *now, const health_state_t *previous);

/**
 * @brief Pack an event frame.
 *
 * @param code health_code_t of the event.
 * @param subject Sent as one byte.
 * @param sequence Only the low 6 bits are sent.
 * @param count Capped at HEALTH_EVENT_COUNT_MAX.
 * @param error Sent as a health_error_t.
 * @param timestamp_ms Master time of the last occurrence.
 * @return The frame, ready to send.
 */
health_event_frame_t node_health_pack_event(uint8_t code, uint32_t subject, uint8_t sequence,
                                            uint32_t count, esp_err_t error, uint32_t timestamp_ms);
