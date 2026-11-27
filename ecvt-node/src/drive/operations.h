// Internal sequences invoked only by the drive-control owner task.
// Frontends use control.h; they must not call these functions concurrently.
#pragma once
#include "esp_err.h"
#include <cstdint>

struct DrivePositionSetup {
    float acceleration_rpm_s = 2300;
    float deceleration_rpm_s = 2300;
    float maximum_rpm = 0;
    uint8_t node_id = 1;
};

// Configure position PDOs/mode and seed measured position before enabling with Halt.
// touched is set before setup: even a failed acknowledgement may change the drive.
esp_err_t drive_prepare_position(const DrivePositionSetup &setup, bool &touched);
// Select mode 1 while disabled. Leaves stored position/trajectory registers intact.
esp_err_t drive_prepare_position_mode();
// Clear velocity target before selecting mode 3; verify disabled/error-free state.
// Never sends a controlword or enables the drive.
esp_err_t drive_prepare_velocity_zero();
// Trigger one absolute position in motor rev at RPM, with acceleration in RPM/s.
// Waits for fresh completion (20 s maximum) or cancellation; owner handles disable.
esp_err_t drive_move_to(float position_revs, float speed_rpm, float acceleration_rpm_s,
                        float deceleration_rpm_s);
// Zero-target mode 3, 0006 -> 0007 -> 000F, ten seconds, then unconditional disable.
// Enables torque; success does not prove mechanical brake release or engagement.
esp_err_t drive_test_brake();
// Experimental mode-4 zero-torque observation, up to 20 s; zero/disable on exit.
esp_err_t drive_test_zero_torque(bool &touched);
// Always attempt 0000 before reading status, even when cancelled or after a reboot.
// Success confirms Switch on disabled, not standstill or physical brake lock.
esp_err_t drive_disable();
bool drive_disable_pending();
