#pragma once
#include <cstdint>

struct ServoFeedback {
    uint16_t statusword = 0;
    int32_t position_counts = 0;
    int32_t velocity_counts = 0;
    int64_t status_us = 0;
    int64_t position_us = 0;
    int64_t velocity_us = 0;
};

void servo_feedback_record(unsigned pdo, int32_t value, int64_t received_us);
ServoFeedback servo_feedback_snapshot();
