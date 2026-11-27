// Public operation API for the servo drive: requests, results and status.
// Frontends submit work to one owner; this header performs no drive I/O.
#pragma once
#include <cstdint>
#include "esp_err.h"
#include "canopen/feedback.h"

enum class DriveCommand {
    Stop,
    ModeManual,
    Limits,
    Profile,
    Move,
    ServoPing,
    ServoInspect,
    ServoErrors,
    ServoResetFault,
    ServoRead,
    BrakeProbe,
    BrakeRestore,
    ServoControlword,
    ServoVelocityZero,
    ServoTorqueZeroTest,
    ServoPositionMode,
    ParameterRead,
    ParameterWrite,
    ParameterSnapshot
};

enum class ObjectWidth : uint8_t { U8 = 1, U16 = 2, U32 = 4 };
struct DriveObject {
    uint16_t index = 0;
    uint8_t subindex = 0;
    ObjectWidth width = ObjectWidth::U32;
    uint32_t value = 0; // Raw bits, never converted through float.
};
constexpr unsigned DRIVE_SNAPSHOT_CAPACITY = 8;
// values: Move={absolute motor rev, RPM}, Limits={min rev, max rev, max RPM},
// Profile={acceleration RPM/s, deceleration RPM/s}, ServoControlword={word}.
// Typed object requests use objects/object_count without float conversion.
struct DriveRequest {
    DriveCommand kind = DriveCommand::Stop;
    float values[3] = {}; // Existing motion units: rev, rpm, rpm/s.
    DriveObject objects[DRIVE_SNAPSHOT_CAPACITY] = {};
    unsigned object_count = 0;
};
struct DriveObjectResult {
    DriveObject object;
    esp_err_t error = ESP_OK;
    int64_t sampled_us = 0;
};
// Completion of an admitted request; cancellation and drive errors remain distinct.
// For snapshots, object_count includes only attempted reads.
struct DriveResult {
    uint32_t id = 0;
    DriveCommand command = DriveCommand::Stop;
    esp_err_t error = ESP_OK;
    bool cancelled = false;
    int64_t completed_us = 0;
    DriveObjectResult objects[DRIVE_SNAPSHOT_CAPACITY] = {};
    unsigned object_count = 0;
};
struct DriveReceipt {
    esp_err_t error = ESP_OK; // Admission only; completion is DriveResult.
    uint32_t id = 0;
};

// Optional completion callback; it runs on the owner task and must not block.
struct DriveObserver {
    void *context = nullptr;
    // Copy the result if it must outlive the callback; never block the owner.
    void (*completed)(void *, const DriveResult &) = nullptr;
};
enum class DriveRunState { Idle, Busy, Enabled, DisableUnconfirmed };
struct DriveStatus {
    DriveRunState state = DriveRunState::Idle;
    uint32_t active_request_id = 0;
    bool busy = false;
    bool disable_pending = false;
    esp_err_t error = ESP_OK;
    DriveFeedback feedback;
    bool limits_configured = false;
    float minimum_revs = 0, maximum_revs = 0, maximum_rpm = 0;
    float acceleration_rpm_s = 2300, deceleration_rpm_s = 2300;
    DriveResult last_result;
};

// Start once after CAN initialization. Starts idle and sends no drive commands.
esp_err_t drive_control_start(const DriveObserver &observer = {});
// Thread-safe admission; a successful receipt is not completion.
// Stop/0000 bypass busy/mode checks, cancel work and coalesce while pending.
DriveReceipt drive_control_submit(const DriveRequest &request);
// Copy cached state, feedback and latest result; this does not read the drive.
DriveStatus drive_control_status();
