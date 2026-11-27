#include "servo_feedback.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE feedback_lock = portMUX_INITIALIZER_UNLOCKED;
static ServoFeedback feedback;

void servo_feedback_record(unsigned pdo, int32_t value, int64_t received_us)
{
    portENTER_CRITICAL(&feedback_lock);
    if (pdo == 1) { feedback.statusword = static_cast<uint16_t>(value); feedback.status_us = received_us; }
    if (pdo == 2) { feedback.position_counts = value; feedback.position_us = received_us; }
    if (pdo == 3) { feedback.velocity_counts = value; feedback.velocity_us = received_us; }
    if (pdo == 4) { feedback.current_raw = static_cast<int16_t>(value); feedback.current_us = received_us; }
    portEXIT_CRITICAL(&feedback_lock);
}

ServoFeedback servo_feedback_snapshot()
{
    portENTER_CRITICAL(&feedback_lock);
    ServoFeedback result = feedback;
    portEXIT_CRITICAL(&feedback_lock);
    return result;
}
