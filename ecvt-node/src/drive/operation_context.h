// C-compatible access to the active operation's cancellation and motion bounds.
// Used by owner-run sequences and the CANopen client, never by frontend handlers.
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool drive_operation_cancelled(void);
// Cooperative owner wait: false means cancellation, not a completed delay.
bool drive_operation_wait_ms(unsigned milliseconds);
bool drive_position_allowed(float position_revs, float speed_rpm);
// Record a driver failure and invalidate the active operation.
void drive_operation_error(esp_err_t error);
#ifdef __cplusplus
}
#endif
