#include "time/gps_time.h"

int64_t gps_time_sample_utc(int64_t utc_now_ms, uint64_t uptime_now_ms,
                            uint32_t sample_ms)
{
    if (utc_now_ms <= 0) return 0;
    uint32_t delta = sample_ms - (uint32_t)uptime_now_ms;
    int64_t signed_delta = delta <= INT32_MAX ? (int64_t)delta : (int64_t)delta - INT64_C(4294967296);
    return utc_now_ms + signed_delta;
}
