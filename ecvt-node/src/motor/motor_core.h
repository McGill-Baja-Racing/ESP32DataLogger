#pragma once
#include "requests.h"
#include "runtime.h"
#include "servo/servo_feedback.h"

// Optional application services. No sensor, logger, terminal or network headers.
// Drive/result callbacks run on the owner and must not issue concurrent drive
// transactions. Recording/status hooks may also run on a frontend task.
struct MotorCoreServices {
    void *context = nullptr;
    esp_err_t (*run_automatic)(void *) = nullptr;
    void (*stop_inputs)(void *) = nullptr; // Nonblocking; before disable.
    void (*wait_inputs)(void *) = nullptr; // After disable has been attempted.
    void (*record_command)(void *, float position_revs, float speed_rpm) = nullptr;
    esp_err_t (*start_recording)(void *) = nullptr;
    void (*stop_recording)(void *) = nullptr;
    bool (*recording_stopped)(void *) = nullptr;
    uint32_t (*dropped_records)(void *) = nullptr;
    // Copy the result if it must outlive the callback; never block the owner.
    void (*completed)(void *, const MotorResult &) = nullptr;
};
enum class MotorRunState { Idle, Busy, Enabled, DisableUnconfirmed };
struct MotorStatus {
    MotorRunState state = MotorRunState::Idle;
    uint32_t active_request_id = 0;
    bool automatic = false;
    bool busy = false;
    bool disable_pending = false;
    esp_err_t error = ESP_OK;
    ServoFeedback feedback;
    bool limits_configured = false;
    float minimum_revs = 0, maximum_revs = 0, maximum_rpm = 0;
    float acceleration_rpm_s = 2300, deceleration_rpm_s = 2300;
    bool recording_stopped = true;
    uint32_t dropped_records = 0;
    MotorResult last_result;
};

// Platform startup initializes the CAN driver/feedback first. Starts idle; no motion.
esp_err_t motor_core_start(const MotorCoreServices &services = {});
// Thread-safe admission. Stop/0000 bypass busy/mode checks. Results are asynchronous.
MotorReceipt motor_core_submit(const MotorRequest &request);
MotorStatus motor_core_status();
// Owner-only measurement hook used by the existing operation runner.
esp_err_t motor_core_start_recording();
void motor_core_stop_recording();
void motor_core_record_command(float position_revs, float speed_rpm);
