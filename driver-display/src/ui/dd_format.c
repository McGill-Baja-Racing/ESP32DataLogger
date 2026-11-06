#include "dd_format.h"

#include <stdio.h>

#define MINUS_SIGN "\xE2\x88\x92" /* U+2212, as wide as the plus sign */

void dd_format_lap_time(char *buffer, size_t size, int32_t milliseconds)
{
    int32_t tenths = (LV_MAX(milliseconds, 0) + 50) / 100;
    snprintf(buffer, size, "%d:%02d.%d", (int)(tenths / 600), (int)(tenths / 10 % 60), (int)(tenths % 10));
}

void dd_format_delta(char *buffer, size_t size, int32_t milliseconds)
{
    int32_t hundredths = (LV_ABS(milliseconds) + 5) / 10;
    const char *sign = milliseconds < 0 ? MINUS_SIGN : "+";
    snprintf(buffer, size, "%s%d.%02d", sign, (int)(hundredths / 100), (int)(hundredths % 100));
}

dd_level_t dd_delta_level(int32_t milliseconds)
{
    if (milliseconds > 0) {
        return DD_LEVEL_CRITICAL;
    }
    return milliseconds < 0 ? DD_LEVEL_GOOD : DD_LEVEL_NORMAL;
}
