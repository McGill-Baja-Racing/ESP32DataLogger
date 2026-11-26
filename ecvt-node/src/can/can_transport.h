#pragma once
#include "app/runtime_internal.h"

bool twai_rx_cb(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *edata,
                       void *user_ctx);

void rx_dispatch_task(void *arg);
