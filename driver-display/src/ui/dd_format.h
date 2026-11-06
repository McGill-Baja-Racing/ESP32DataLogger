#ifndef DD_FORMAT_H
#define DD_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "dd_theme.h"

/* Lap time in milliseconds as "2:14.6", rounded to tenths. */
void dd_format_lap_time(char *buffer, size_t size, int32_t milliseconds);

/* Gap to a reference lap as "+0.36" or "−0.84" (true minus sign), rounded to
 * hundredths. Positive means slower. */
void dd_format_delta(char *buffer, size_t size, int32_t milliseconds);

/* Slower than the reference is critical (red), faster is good (green). */
dd_level_t dd_delta_level(int32_t milliseconds);

#endif /* DD_FORMAT_H */
