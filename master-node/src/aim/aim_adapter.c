#include "aim/aim_adapter.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* CAN dispatch writes this state while web/serial tasks read it. */
static aim_tracker_t tracker;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

bool aim_adapter_receive(const can_message_t *frame, can_message_t *sample)
{
    portENTER_CRITICAL(&lock);
    bool valid = aim_decode(&tracker, frame, sample);
    portEXIT_CRITICAL(&lock);
    return valid;
}

void aim_adapter_status(aim_status_t *status)
{
    portENTER_CRITICAL(&lock);
    *status = aim_tracker_status(&tracker, esp_timer_get_time());
    portEXIT_CRITICAL(&lock);
}
