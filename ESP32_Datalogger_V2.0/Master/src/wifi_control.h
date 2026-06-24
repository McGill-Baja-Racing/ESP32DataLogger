#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_control_start(void);
esp_err_t wifi_control_reconnect(const char *reason);
bool wifi_control_is_connected(void);
