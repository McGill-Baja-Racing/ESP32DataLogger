#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t utc_ms;
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint16_t speed_kph_x100;
    bool has_location;
    bool has_speed;
    bool has_utc;
} gps_rmc_fix_t;

/**
 * @brief Parse one line of text from the GPS.
 *
 * The GPS sends several kinds of lines; this reads the RMC line, which
 * carries time, position and speed. A value the GPS left out clears its
 * has_ flag.
 *
 * @param line Line from the GPS, without the line ending.
 * @param fix  Set to the parsed values; cleared on failure.
 * @return false if the line is corrupted, is not an RMC line, or the GPS
 *         has no position yet.
 */
bool gps_rmc_parse(const char *line, gps_rmc_fix_t *fix);
