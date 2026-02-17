#ifndef NODE_HPP
#define NODE_HPP

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <iostream>
#include <string>

class Node{

    Node(const std::string &tag, const int id, twai_onchip_node_config_t node_config);
    ~Node();
    void can_transmit_frame(const twai_frame_t *frame, int timeout_ms);
    void can_register_event_callbacks(twai_event_callbacks_t callback_event, void * user_data);
    twai_node_handle_t node_hdl = NULL;
};
#endif