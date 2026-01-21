/*
twai_mask_filter_config_t mfilter_cfg = {
    .id = 0x10,         // 0b 000 0001 0000
    .mask = 0x7f0,      // 0b 111 1111 0000 — the upper 7 bits must match strictly, the lower 4 bits are ignored, accepts IDs of the form
                        // 0b 000 0001 xxxx (hex 0x01x)
    .is_ext = false,    // Accept only standard IDs, not extended IDs
};
ESP_ERROR_CHECK(twai_node_config_mask_filter(node_hdl, 0, &mfilter_cfg))*/