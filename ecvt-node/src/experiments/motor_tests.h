#pragma once
#include "console/command_parser.h"
#include "esp_err.h"

// The application owns the drive and prepares position mode before calling.
esp_err_t motor_test_run(CommandKind kind, float position, float rpm, float accel, float decel);
