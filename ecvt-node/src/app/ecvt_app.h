#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
bool ecvt_app_stop_requested(void);
void ecvt_app_motor_error(esp_err_t error);
bool ecvt_app_allow_move(float position_revs, float speed_rpm);
#ifdef __cplusplus
}

#include "console/command_parser.h"
esp_err_t ecvt_app_start(bool sd_available);
void ecvt_app_submit(const ConsoleCommand &command);
bool ecvt_app_wait_ms(unsigned milliseconds);
void ecvt_app_print_status();
#endif
