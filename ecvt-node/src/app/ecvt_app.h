#pragma once
#include "esp_err.h"
#include "console/command_parser.h"
#include "motor/requests.h"
esp_err_t ecvt_app_start();
bool ecvt_app_motor_request(const ConsoleCommand &command, MotorRequest *request);
void ecvt_app_submit(const ConsoleCommand &command);
void ecvt_app_print_status();
