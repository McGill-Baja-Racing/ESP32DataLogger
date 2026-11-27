#pragma once
#include "motor/requests.h"
#include "esp_err.h"

// The application owns the drive and prepares position mode before calling.
esp_err_t motor_test_run(MotorCommand kind, float position, float rpm, float accel, float decel);
