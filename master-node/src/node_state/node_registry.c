#include "node_state/node_registry.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    bool changed = !nodes[id].seen || nodes[id].offline ||
                   nodes[id].state != state ||
                   nodes[id].reason != reason ||
                   nodes[id].reset_reason != reset_reason;
    nodes[id].seen = true;
    nodes[id].state = state;
    nodes[id].reason = reason;
    nodes[id].reset_reason = reset_reason;
    if (changed) {
        ESP_LOGI(TAG, "Node %" PRIu32 " state=%s reason=%u reset=%u", id,
                 nodes[id].state == NODE_STATE_ACTIVE ? "active" : "idle",
                 nodes[id].reason, nodes[id].reset_reason);
    }
}

void node_registry_set_monitoring(bool enabled)
{
    monitoring = enabled;
    TickType_t now = xTaskGetTickCount();
    for (uint32_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        if (!is_configured_node(id)) continue;
        nodes[id].data_seen = false;
        nodes[id].last_seen = now;
        nodes[id].offline = false;
    }
}

/* Any frame proves the node is alive; only sensor frames prove data flows. */
static void record_frame(uint8_t id, bool sensor_data)
{
    if (!monitoring || id == 0 || id >= CAN_NODE_SLOT_COUNT) return;
    bool reconnected = nodes[id].offline;
    TickType_t now = xTaskGetTickCount();
    if (sensor_data) {
        nodes[id].data_seen = true;
        nodes[id].last_data = now;
    }
    nodes[id].last_seen = now;
    nodes[id].offline = false;
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
        for (uint32_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
            if (!is_configured_node(id)) continue;
            if (monitoring && !nodes[id].offline &&
                now - nodes[id].last_seen >=
                    pdMS_TO_TICKS(NODE_OFFLINE_TIMEOUT_MS)) {
                nodes[id].offline = true;
                ESP_LOGW(TAG, "Node %" PRIu32 " offline; no frames for %u ms",
                         id, NODE_OFFLINE_TIMEOUT_MS);
            }
        }
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
    if (!monitoring) return "off";
    if (nodes[node_id].offline) return "offline";
    bool data_recent = nodes[node_id].data_seen &&
                       xTaskGetTickCount() - nodes[node_id].last_data <
                           pdMS_TO_TICKS(NODE_OFFLINE_TIMEOUT_MS);
    return data_recent ? "active" : "waiting";
}
