#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool motor_operation_cancelled(void);
bool motor_operation_wait_ms(unsigned milliseconds);
bool motor_position_allowed(float position_revs, float speed_rpm);
void motor_operation_error(esp_err_t error);
#ifdef __cplusplus
}
#endif
