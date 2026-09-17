#include "time/gps_time.h"
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int digits(const char *s, unsigned n)
{
    int value = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        value = value * 10 + s[i] - '0';
    }
    return value;
}
static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

bool gps_time_parse_rmc(const char *line, int64_t *utc_ms)
{
    if (!line || !utc_ms || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || strlen(star) != 3) return false;
    int hi = hex(star[1]), lo = hex(star[2]);
    if (hi < 0 || lo < 0) return false;
    unsigned checksum = 0;
    for (const char *s = line + 1; s < star; ++s) checksum ^= (unsigned char)*s;
    if (checksum != (unsigned)(hi * 16 + lo)) return false;
    char fields[10][24] = {{0}};
    const char *s = line + 1;
    for (unsigned i = 0; i < 10; ++i) {
        const char *end = s;
        while (end < star && *end != ',') ++end;
        size_t n = (size_t)(end - s);
        if (n >= sizeof(fields[i])) return false;
        memcpy(fields[i], s, n);
        if (i < 9 && end == star) return false;
        s = end + 1;
    }
    if (strlen(fields[0]) != 5 || strcmp(fields[0] + 2, "RMC") ||
        strcmp(fields[2], "A") || strlen(fields[9]) != 6) return false;
    const char *t = fields[1];
    size_t n = strlen(t);
    if (n < 6 || (n > 6 && (t[6] != '.' || n == 7))) return false;
    int h = digits(t, 2), m = digits(t + 2, 2), sec = digits(t + 4, 2);
    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
    int ms = 0, scale = 100;
    for (size_t i = 7; i < n; ++i) {
        int d = digits(t + i, 1);
        if (d < 0) return false;
        ms += d * scale;
        scale /= 10;
    }
    int day = digits(fields[9], 2), month = digits(fields[9] + 2, 2);
    int yy = digits(fields[9] + 4, 2), year = 2000 + yy;
    if (yy < 0 || year < 2020 || month < 1 || month > 12) return false;
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (day < 1 || day > lengths[month - 1] + (month == 2 && leap(year))) return false;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += 365 + leap(y);
    for (int mo = 1; mo < month; ++mo) days += lengths[mo - 1] + (mo == 2 && leap(year));
    days += day - 1;
    *utc_ms = ((days * 24 + h) * 3600 + m * 60 + sec) * 1000 + ms;
    return true;
}

int64_t gps_time_sample_utc(int64_t utc_now_ms, uint64_t uptime_now_ms,
                            uint32_t sample_ms)
{
    if (utc_now_ms <= 0) return 0;
    uint32_t delta = sample_ms - (uint32_t)uptime_now_ms;
    int64_t signed_delta = delta <= INT32_MAX ? (int64_t)delta : (int64_t)delta - INT64_C(4294967296);
    return utc_now_ms + signed_delta;
}
