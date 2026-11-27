#pragma once
#include "requests.h"
#include "runtime.h"
#include "servo/servo_feedback.h"

// Optional nonblocking result observer. No frontend or system dependencies.
struct MotorCoreServices {
    void *context = nullptr;
    // Copy the result if it must outlive the callback; never block the owner.
    void (*completed)(void *, const MotorResult &) = nullptr;
};
enum class MotorRunState { Idle, Busy, Enabled, DisableUnconfirmed };
struct MotorStatus {
    MotorRunState state = MotorRunState::Idle;
    uint32_t active_request_id = 0;
    bool busy = false;
    bool disable_pending = false;
    esp_err_t error = ESP_OK;
    ServoFeedback feedback;
    bool limits_configured = false;
    float minimum_revs = 0, maximum_revs = 0, maximum_rpm = 0;
    float acceleration_rpm_s = 2300, deceleration_rpm_s = 2300;
    MotorResult last_result;
};

// Platform startup initializes the CAN driver/feedback first. Starts idle; no motion.
esp_err_t motor_core_start(const MotorCoreServices &services = {});
// Thread-safe admission. Stop/0000 bypass busy/mode checks. Results are asynchronous.
MotorReceipt motor_core_submit(const MotorRequest &request);
MotorStatus motor_core_status();
