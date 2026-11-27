// Drive feedback cache in raw CANopen units with local receive timestamps.
// A snapshot copies the cache; its fields are not a simultaneous physical sample.
#pragma once
#include <cstdint>

struct DriveFeedback {
    uint16_t statusword = 0;
    int32_t position_counts = 0;
    int32_t velocity_counts = 0;
    int16_t current_raw = 0;
    int64_t current_us = 0;
    int64_t status_us = 0;
    int64_t position_us = 0;
    int64_t velocity_us = 0;
};

// RX task records each mapped TPDO without issuing a drive transaction.
void drive_feedback_record(unsigned pdo, int32_t value, int64_t received_us);
// Thread-safe copy; use each field's timestamp when checking freshness.
DriveFeedback drive_feedback_snapshot();
