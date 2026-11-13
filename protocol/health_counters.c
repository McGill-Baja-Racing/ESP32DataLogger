#include "protocol/health_counters.h"

#include <stddef.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define RESET_COUNT_MAGIC 0xB4A7DA03u

static health_state_t state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
/* Kept through every reset but power-on; the magic tells a kept count from
 * power-on garbage. */
static RTC_NOINIT_ATTR uint32_t reset_count_magic;
static RTC_NOINIT_ATTR uint32_t reset_count;

/**
 * @brief Tell whether a code keeps a separate count for each subject.
 *
 * @param code Health code.
 * @return true for codes whose subject is a CAN ID, node or GPIO.
 */
static bool counted_per_subject(health_code_t code)
{
    return code == HEALTH_SAMPLES_MISSED || code == HEALTH_SAMPLES_DROPPED ||
           code == HEALTH_BAD_TIMESTAMP || code == HEALTH_CAN_TX ||
           code == HEALTH_CAN_RX_OVERFLOW || code == HEALTH_EVENT_LOST ||
           code == HEALTH_ADC_UNCALIBRATED;
}

/**
 * @brief Find the counter for a code and subject. Call under lock.
 *
 * Counter N is code N's, for its first subject. Other subjects take shared
 * counters in order, never freed; when none is left they join the code's own.
 *
 * @param code Health code.
 * @param subject CAN ID, node or GPIO the count is for.
 * @return The counter to add to; never NULL.
 */
static health_counter_t *counter_for(health_code_t code, uint32_t subject)
{
    health_counter_t *own = &state.counters[code];
    if (!counted_per_subject(code) || own->count == 0 || own->subject == subject) {
        return own;
    }
    for (size_t i = HEALTH_CODE_COUNT; i < HEALTH_COUNTER_COUNT; i++) {
        health_counter_t *counter = &state.counters[i];
        if (counter->count == 0 || (counter->code == code && counter->subject == subject)) {
            return counter;
        }
    }
    return own;
}

void node_health_count(health_code_t code, uint32_t amount, uint32_t subject,
                       esp_err_t error)
{
    if (amount == 0) {
        return;
    }
    subject = protocol_health_subject(subject);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL_SAFE(&lock);
    health_counter_t *counter = counter_for(code, subject);
    counter->code = code;
    counter->count += amount;
    counter->subject = subject;
    counter->error = error;
    counter->last_ms = now;
    portEXIT_CRITICAL_SAFE(&lock);
}

void node_health_set(health_code_t fault, bool active)
{
    portENTER_CRITICAL_SAFE(&lock);
    bool onset = active && !state.active[fault];
    state.active[fault] = active;
    portEXIT_CRITICAL_SAFE(&lock);
    if (onset) {
        node_health_count(fault, 1, 0, ESP_OK);
    }
}

void node_health_copy_counts(health_state_t *copy)
{
    portENTER_CRITICAL(&lock);
    *copy = state;
    portEXIT_CRITICAL(&lock);
}

uint32_t node_health_get_faults(const health_state_t *now, const health_state_t *previous)
{
    uint32_t faults = 0;
    for (int fault = 0; fault < HEALTH_FAULT_COUNT; fault++) {
        if (now->active[fault]) {
            faults |= HEALTH_BIT(fault);
        }
    }
    for (size_t i = 0; i < HEALTH_COUNTER_COUNT; i++) {
        if (now->counters[i].count != previous->counters[i].count &&
            now->counters[i].code < HEALTH_FAULT_COUNT) {
            faults |= HEALTH_BIT(now->counters[i].code);
        }
    }
    return faults;
}

/**
 * @brief Map an ESP-IDF error to the 4-bit error of an event frame.
 *
 * @param error ESP-IDF error, such as ESP_ERR_TIMEOUT.
 * @return The matching health_error_t, or HEALTH_ERROR_OTHER.
 */
static health_error_t map_error(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return HEALTH_ERROR_NONE;
    case ESP_FAIL: return HEALTH_ERROR_FAIL;
    case ESP_ERR_NO_MEM: return HEALTH_ERROR_NO_MEM;
    case ESP_ERR_INVALID_ARG: return HEALTH_ERROR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return HEALTH_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return HEALTH_ERROR_NOT_FOUND;
    case ESP_ERR_TIMEOUT: return HEALTH_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_RESPONSE: return HEALTH_ERROR_INVALID_RESPONSE;
    default: return HEALTH_ERROR_OTHER;
    }
}

health_event_frame_t node_health_pack_event(uint8_t code, uint32_t subject, uint8_t sequence,
                                            uint32_t count, esp_err_t error, uint32_t timestamp_ms)
{
    health_event_frame_t event = {
        .code = code,
        .subject = (uint8_t)subject,
        .sequence = sequence & HEALTH_EVENT_SEQUENCE_MASK,
        .count = count > HEALTH_EVENT_COUNT_MAX ? HEALTH_EVENT_COUNT_MAX : count,
        .error = map_error(error),
        .timestamp_ms = timestamp_ms,
    };
    return event;
}

void node_health_count_boot(uint8_t *resets, uint8_t *reset_reason)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
        reset_count = 0;
    } else if (reset_count_magic != RESET_COUNT_MAGIC) {
        reset_count = 1;
    } else if (reset_count < UINT8_MAX) {
        reset_count++;
    }
    reset_count_magic = RESET_COUNT_MAGIC;
    *resets = (uint8_t)reset_count;
    *reset_reason = (uint8_t)reason;
    node_health_count(HEALTH_BOOT, 1, reason, ESP_OK);
}
