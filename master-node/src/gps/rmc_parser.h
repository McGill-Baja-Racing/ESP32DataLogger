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

/* Checksum-valid active RMC, without CR/LF. Reject malformed telemetry;
 * missing telemetry and invalid UTC are represented by the availability flags.
 * Output is cleared on failure. The UART reader bounds sentence length. */
bool gps_rmc_parse(const char *line, gps_rmc_fix_t *fix);
