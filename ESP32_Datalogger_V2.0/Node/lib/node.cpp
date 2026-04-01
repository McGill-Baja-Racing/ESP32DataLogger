#include "node.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_twai.h"

//TODO TEST
Node::Node(const std::string& tag, const int id, twai_onchip_node_config_t node_config){
    /* ---------- Config Driver      ---------- */
    esp_err_t err = twai_new_node_onchip(&node_config, &node_hdl);
    if (err != ESP_OK) {
        ESP_LOGE("TWAI", "Failed to initialize TWAI node: %s", esp_err_to_name(err));
        return;
    }
    /* ---------- Enable / start TWAI ---------- */
    err = twai_node_enable(node_hdl);
    if (err != ESP_OK) {
        ESP_LOGE("TWAI", "Failed to enable TWAI node: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI("TWAI", "TWAI node configured and enabled successfully!");

}

void Node::can_register_event_callbacks(twai_event_callbacks_t callbacks, void *user_data){
    esp_err_t err = twai_node_register_event_callbacks(node_hdl, &callbacks, user_data);
}

void Node::can_transmit_frame(const twai_frame_t *frame, int timeout_ms){

    esp_err_t err = twai_node_transmit(node_hdl, frame, timeout_ms); // handle error
    if (err != ESP_OK) {
        ESP_LOGE("TWAI", "Failed to transmit frame: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI("TWAI", "Frame transmitted successfully.");
    }
}
Node::~Node() {
    // Disable the TWAI node
    if (node_hdl != NULL) {
        esp_err_t err = twai_node_disable(node_hdl);
        if (err != ESP_OK) {
            ESP_LOGE("TWAI", "Failed to disable TWAI node: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI("TWAI", "TWAI node disabled successfully.");
        }
    }
    // Additional cleanup (if necessary)
    node_hdl = NULL;  // Optional: reset the handle to NULL after cleanup
}