#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Checksum-valid, active RMC with a plausible UTC calendar date. */
bool gps_time_parse_rmc(const char *line, int64_t *utc_ms);
/* Sample timestamps share the master's wrapping 32-bit millisecond clock. */
int64_t gps_time_sample_utc(int64_t utc_now_ms, uint64_t uptime_now_ms,
                            uint32_t sample_ms);
