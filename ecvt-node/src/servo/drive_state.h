#pragma once
#include "esp_err.h"

// Owner-task only. Always attempt 6040=0000 before reading status; no mode,
// velocity, cancellation or remembered-state prerequisite. Never sends enable.
// Success confirms Switch on disabled, not standstill or physical brake lock.
esp_err_t servo_disable_and_confirm();
