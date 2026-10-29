#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t time_beacon_start(void);
void time_beacon_set_recording(bool recording);
