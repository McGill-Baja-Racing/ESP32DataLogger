#pragma once
#include <cstdint>
#include "esp_err.h"

enum class MotorCommand {
    Stop, ModeManual, Limits, Profile, Move,
    ServoPing, ServoInspect, ServoErrors, ServoResetFault, ServoRead,
    BrakeProbe, BrakeRestore, ServoControlword,
    ServoVelocityZero, ServoTorqueZeroTest, ServoPositionMode,
    ParameterRead, ParameterWrite, ParameterSnapshot
};

enum class ObjectWidth : uint8_t { U8=1, U16=2, U32=4 };
struct MotorObject {
    uint16_t index = 0;
    uint8_t subindex = 0;
    ObjectWidth width = ObjectWidth::U32;
    uint32_t value = 0; // Raw bits, never converted through float.
};
constexpr unsigned MOTOR_SNAPSHOT_CAPACITY = 8;
struct MotorRequest {
    MotorCommand kind = MotorCommand::Stop;
    float values[3] = {}; // Existing motion units: rev, rpm, rpm/s.
    MotorObject objects[MOTOR_SNAPSHOT_CAPACITY] = {};
    unsigned object_count = 0;
};
struct MotorObjectResult {
    MotorObject object;
    esp_err_t error = ESP_OK;
    int64_t sampled_us = 0;
};
struct MotorResult {
    uint32_t id = 0;
    MotorCommand command = MotorCommand::Stop;
    esp_err_t error = ESP_OK;
    bool cancelled = false;
    int64_t completed_us = 0;
    MotorObjectResult objects[MOTOR_SNAPSHOT_CAPACITY] = {};
    unsigned object_count = 0;
};
struct MotorReceipt {
    esp_err_t error = ESP_OK; // Admission only; completion is MotorResult.
    uint32_t id = 0;
};
