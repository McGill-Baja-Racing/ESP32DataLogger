#include "node_state/node_registry.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health/node_health.h"
#include "protocol/app_protocol.h"

#define NODE_OFFLINE_TIMEOUT_MS 3000
#define NODE_MONITOR_PERIOD_MS  250

_Static_assert((MASTER_EXPECTED_NODE_MASK & ~CAN_NODE_ID_MASK) == 0, "MASTER_EXPECTED_NODE_MASK has a node missing from CAN_NODE_ID_MASK");

typedef struct {
    bool seen;
    bool data_seen;
    uint8_t state;
    uint8_t reason;
    uint8_t reset_reason;
    TickType_t last_seen;
    TickType_t last_data;
    bool offline;
} registered_node_t;

static const char *TAG = "NodeRegistry";
static registered_node_t nodes[CAN_NODE_SLOT_COUNT];
static bool monitoring;
static portMUX_TYPE registry_lock = portMUX_INITIALIZER_UNLOCKED;

static bool is_configured_node(uint32_t id)
{
    return id < 32 && (MASTER_EXPECTED_NODE_MASK & (1U << id)) != 0;
}

bool node_registry_is_state_frame(const can_message_t *message)
{
    return message->id > CAN_ID_NODE_STATE_BASE &&
           message->id < CAN_ID_NODE_STATE_BASE + CAN_NODE_SLOT_COUNT;
}

void node_registry_update(const can_message_t *message)
{
    uint32_t id = message->id - CAN_ID_NODE_STATE_BASE;
    if (id >= CAN_NODE_SLOT_COUNT || message->dlc < 2) return;
    uint8_t state = (uint8_t)message->data;
    uint8_t reason = (uint8_t)(message->data >> 8);
    uint8_t reset_reason = message->dlc >= 3
                         ? (uint8_t)(message->data >> 16) : 0;
    portENTER_CRITICAL(&registry_lock);
    bool changed = !nodes[id].seen || nodes[id].offline ||
                   nodes[id].state != state ||
                   nodes[id].reason != reason ||
                   nodes[id].reset_reason != reset_reason;
    nodes[id].seen = true;
    nodes[id].state = state;
    nodes[id].reason = reason;
    nodes[id].reset_reason = reset_reason;
    portEXIT_CRITICAL(&registry_lock);
    if (changed) {
        ESP_LOGI(TAG, "Node %" PRIu32 " state=%s reason=%u reset=%u", id,
                 state == NODE_STATE_ACTIVE ? "active" : "idle",
                 reason, reset_reason);
    }
}

void node_registry_set_monitoring(bool enabled)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&registry_lock);
    monitoring = enabled;
    for (uint32_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if (!is_configured_node(id)) continue;
        nodes[id].data_seen = false;
        nodes[id].last_seen = now;
        nodes[id].offline = false;
    }
    portEXIT_CRITICAL(&registry_lock);
}

/* Any frame proves the node is alive; only sensor frames prove data flows. */
static void record_frame(uint8_t id, bool sensor_data)
{
    if (id == 0 || id >= CAN_NODE_SLOT_COUNT) return;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&registry_lock);
    if (!monitoring) {
        portEXIT_CRITICAL(&registry_lock);
        return;
    }
    bool reconnected = nodes[id].offline;
    if (sensor_data) {
        nodes[id].data_seen = true;
        nodes[id].last_data = now;
    }
    nodes[id].last_seen = now;
    nodes[id].offline = false;
    portEXIT_CRITICAL(&registry_lock);
    if (reconnected) {
        ESP_LOGI(TAG, "Node %u back online", id);
    }
}

void node_registry_record_sensor_frame(const can_message_t *message)
{
    record_frame(protocol_sensor_node_id(message->id), true);
}

void node_registry_record_health_frame(uint8_t node_id)
{
    record_frame(node_id, false);
}

static void monitor_task(void *argument)
{
    (void)argument;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        uint32_t went_offline = 0;
        uint32_t offline_mask = 0;
        for (uint32_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
            if (!is_configured_node(id)) continue;
            bool became_offline = false;
            bool is_offline;
            portENTER_CRITICAL(&registry_lock);
            if (monitoring && !nodes[id].offline &&
                /* Signed: dispatch can record a frame after now was read. */
                (int32_t)(now - nodes[id].last_seen) >=
                    (int32_t)pdMS_TO_TICKS(NODE_OFFLINE_TIMEOUT_MS)) {
                nodes[id].offline = true;
                became_offline = true;
            }
            is_offline = nodes[id].offline;
            portEXIT_CRITICAL(&registry_lock);
            if (became_offline) {
                went_offline++;
                ESP_LOGW(TAG, "Node %" PRIu32 " offline; no frames for %u ms",
                         id, NODE_OFFLINE_TIMEOUT_MS);
            }
            if (is_offline) offline_mask |= 1U << id;
        }
        node_health_count(HEALTH_NODE_OFFLINE, went_offline, offline_mask, ESP_OK);
        vTaskDelay(pdMS_TO_TICKS(NODE_MONITOR_PERIOD_MS));
    }
}

esp_err_t node_registry_init(void)
{
    return xTaskCreate(monitor_task, "node_monitor", 3072, NULL, 6, NULL) == pdPASS
         ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *node_registry_state_name(uint8_t node_id)
{
    if (node_id >= CAN_NODE_SLOT_COUNT) return "unknown";
    if (!is_configured_node(node_id)) return "disabled";
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&registry_lock);
    bool enabled = monitoring;
    bool offline = nodes[node_id].offline;
    bool data_recent = nodes[node_id].data_seen &&
                       (int32_t)(now - nodes[node_id].last_data) <
                           (int32_t)pdMS_TO_TICKS(NODE_OFFLINE_TIMEOUT_MS);
    portEXIT_CRITICAL(&registry_lock);
    if (!enabled) return "off";
    if (offline) return "offline";
    return data_recent ? "active" : "waiting";
}
