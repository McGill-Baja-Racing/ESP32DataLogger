#include "live/live_data.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define LIVE_DATA_LEASE_US ((int64_t)LIVE_DATA_LEASE_SECONDS * 1000000LL)

static const live_signal_metadata_t signals[LIVE_DATA_SIGNAL_COUNT] = {
    {0x0B1, "front_brake_pressure", "brake_node_1", "psi", 100},
    {0x0B2, "rear_brake_pressure", "brake_node_1", "psi", 100},
    {0x0B9, "bearing_rpm", "encoder_node_4", "rpm", 50},
    {0x0BA, "generic_adc_voltage", "adc_node_6", "mV", 100},
    {0x0BB, "engine_rpm", "engine_node_5", "rpm", 100},
    {0x0BC, "engine_spark", "engine_node_5", "event", 100},
    {0x700, "gps_speed", "master_gps", "km/h_x100", 1},
    {0x701, "gps_latitude", "master_gps", "deg_e7", 1},
    {0x702, "gps_longitude", "master_gps", "deg_e7", 1},
};

static live_sample_t latest[LIVE_DATA_SIGNAL_COUNT];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool enabled;
static uint32_t owner_token;
static int64_t lease_deadline_us;

static int signal_index(uint32_t can_id)
{
    for (size_t i = 0; i < LIVE_DATA_SIGNAL_COUNT; i++) {
        if (signals[i].can_id == can_id) return (int)i;
    }
    return -1;
}

static void clear_locked(void)
{
    enabled = false;
    owner_token = 0;
    lease_deadline_us = 0;
    memset(latest, 0, sizeof(latest));
}

static void expire_locked(int64_t now_us)
{
    if (enabled && now_us >= lease_deadline_us) clear_locked();
}

esp_err_t live_data_init(void)
{
    portENTER_CRITICAL(&lock);
    clear_locked();
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}

const live_signal_metadata_t *live_data_signals(size_t *count)
{
    if (count) *count = LIVE_DATA_SIGNAL_COUNT;
    return signals;
}

void live_data_record(const can_message_t *message)
{
    int index = message ? signal_index(message->id) : -1;
    if (index < 0 || message->dlc < 8) return;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire_locked(now_us);
    if (enabled) {
        live_sample_t *sample = &latest[index];
        sample->can_id = message->id;
        sample->value = (int32_t)(message->data & UINT32_MAX);
        sample->timestamp_ms = (uint32_t)(message->data >> 32);
        sample->sequence++;
        sample->valid = true;
    }
    portEXIT_CRITICAL(&lock);
}

live_data_result_t live_data_start(uint32_t *token)
{
    if (!token) return LIVE_DATA_DISABLED;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire_locked(now_us);
    if (enabled) {
        portEXIT_CRITICAL(&lock);
        return LIVE_DATA_BUSY;
    }
    memset(latest, 0, sizeof(latest));
    owner_token = esp_random();
    if (owner_token == 0) owner_token = 1;
    lease_deadline_us = now_us + LIVE_DATA_LEASE_US;
    enabled = true;
    *token = owner_token;
    portEXIT_CRITICAL(&lock);
    return LIVE_DATA_OK;
}

live_data_result_t live_data_snapshot(uint32_t token, live_sample_t *samples,
                                     size_t capacity, size_t *count)
{
    if (!samples || !count || capacity < LIVE_DATA_SIGNAL_COUNT) {
        return LIVE_DATA_DISABLED;
    }
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire_locked(now_us);
    if (!enabled) {
        portEXIT_CRITICAL(&lock);
        return LIVE_DATA_DISABLED;
    }
    if (token == 0 || token != owner_token) {
        portEXIT_CRITICAL(&lock);
        return LIVE_DATA_INVALID_TOKEN;
    }
    memcpy(samples, latest, sizeof(latest));
    *count = LIVE_DATA_SIGNAL_COUNT;
    lease_deadline_us = now_us + LIVE_DATA_LEASE_US;
    portEXIT_CRITICAL(&lock);
    return LIVE_DATA_OK;
}

live_data_result_t live_data_stop(uint32_t token)
{
    portENTER_CRITICAL(&lock);
    expire_locked(esp_timer_get_time());
    if (!enabled) {
        portEXIT_CRITICAL(&lock);
        return LIVE_DATA_DISABLED;
    }
    if (token == 0 || token != owner_token) {
        portEXIT_CRITICAL(&lock);
        return LIVE_DATA_INVALID_TOKEN;
    }
    clear_locked();
    portEXIT_CRITICAL(&lock);
    return LIVE_DATA_OK;
}

void live_data_force_stop(void)
{
    portENTER_CRITICAL(&lock);
    clear_locked();
    portEXIT_CRITICAL(&lock);
}

bool live_data_is_enabled(void)
{
    portENTER_CRITICAL(&lock);
    expire_locked(esp_timer_get_time());
    bool result = enabled;
    portEXIT_CRITICAL(&lock);
    return result;
}
