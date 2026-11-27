#pragma once
#include "esp_err.h"
#include <cstdint>

struct MotorPositionSetup {
    float acceleration_rpm_s=2300;
    float deceleration_rpm_s=2300;
    float maximum_rpm=0;
    uint8_t node_id=1;
};
// Owner-only drive sequences. touched reports whether setup may have changed
// the drive even when an acknowledgement fails. No console/legacy dependency.
esp_err_t motor_prepare_position(const MotorPositionSetup &, bool &touched);
esp_err_t motor_prepare_position_mode();
esp_err_t motor_torque_zero_brake_test(bool &touched);
