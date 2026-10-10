#pragma once

#include <stdint.h>

/* Maps a raw ADC voltage in mV to pedal position, 0 to 100 percent. */
int32_t brake_position_normalize(int32_t raw_mv);

/* Returns a NUL-terminated greeting. */
const char *brake_position_hello(void);
