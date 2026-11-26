#pragma once
#include "esp_err.h"

// Owner-task only: requires Switch on disabled and no error register.
// Writes zero target velocity, then mode 3; never writes a controlword/NMT.
esp_err_t servo_prepare_velocity_zero();
