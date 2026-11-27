#pragma once
#include "requests.h"

// Owner-only helpers. Frontends use motor_core_submit, never these helpers.
// A snapshot is sequential, not an atomic drive measurement.
esp_err_t motor_read_object(const MotorObject &object, MotorObjectResult &result);
esp_err_t motor_write_object(const MotorObject &object, MotorObjectResult &result);
esp_err_t motor_read_snapshot(const MotorObject *objects, unsigned count, MotorResult &result);
