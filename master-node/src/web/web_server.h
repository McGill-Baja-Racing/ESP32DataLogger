#pragma once

#include "esp_err.h"

/* Starts the noncritical BajaDAQ SoftAP and HTTP control server. */
esp_err_t web_server_start(void);
