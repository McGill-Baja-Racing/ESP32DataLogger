// Startup boundary for the standalone servo CAN bus, independent of DAQ traffic.
#pragma once
#include "esp_err.h"

// Standalone servo bus: TX=5, RX=4, 500 kbit/s, drive node 1.
// Creates the receiver and driver; never sends an enable or motion command.
esp_err_t servo_can_start();
