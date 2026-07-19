#include "node_state/node_registry.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "protocol/app_protocol.h"

#define NODE_SLOT_COUNT 7

typedef struct {
    bool seen;
    uint8_t state;
    uint8_t reason;
    uint8_t reset_reason;
} registered_node_t;

static const char *TAG = "NodeRegistry";
static registered_node_t nodes[NODE_SLOT_COUNT];

bool node_registry_is_state_frame(const can_message_t *message)
{
    return message->id > CAN_ID_NODE_STATE_BASE &&
           message->id < CAN_ID_NODE_STATE_BASE + NODE_SLOT_COUNT;
}

void node_registry_update(const can_message_t *message)
{
    uint32_t id = message->id - CAN_ID_NODE_STATE_BASE;
    if (id >= NODE_SLOT_COUNT || message->dlc < 2) return;
    nodes[id] = (registered_node_t) {
        .seen = true,
        .state = (uint8_t)message->data,
        .reason = (uint8_t)(message->data >> 8),
        .reset_reason = message->dlc >= 3 ? (uint8_t)(message->data >> 16) : 0,
    };
    ESP_LOGI(TAG, "Node %" PRIu32 " state=%s reason=%u reset=%u", id,
             nodes[id].state == PROTOCOL_NODE_ACTIVE ? "active" : "idle",
             nodes[id].reason, nodes[id].reset_reason);
}

const char *node_registry_state_name(uint8_t node_id)
{
    if (node_id >= NODE_SLOT_COUNT || !nodes[node_id].seen) return "unknown";
    return nodes[node_id].state == PROTOCOL_NODE_ACTIVE ? "active" : "idle";
}
