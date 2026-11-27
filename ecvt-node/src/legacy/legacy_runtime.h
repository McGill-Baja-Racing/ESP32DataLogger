#pragma once
#include "motor/motor_core.h"

// Application composition only. The motor core does not include this adapter.
MotorCoreServices legacy_motor_services(bool storage_available);
