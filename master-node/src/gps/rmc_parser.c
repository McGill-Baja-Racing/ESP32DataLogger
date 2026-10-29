#include "gps/rmc_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Validate the whole numeric field before conversion: strtod alone accepts
 * signs, exponents, NaN, infinity, and numeric prefixes followed by garbage. */
static bool decimal(const char *text, double *value)
{
    const char *p = text;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) ++p;
    if (*p == '.') {
        ++p;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p) return false;
    *value = strtod(text, NULL);
    return true;
}

static bool coordinate(const char *text, const char *hemisphere, bool latitude,
                       int32_t *result)
{
    size_t width = latitude ? 4 : 5;
    size_t length = strlen(text);
    if (length < width || (length > width && text[width] != '.')) return false;
    for (size_t i = 0; i < width; ++i) {
        if (!isdigit((unsigned char)text[i])) return false;
    }
    char positive = latitude ? 'N' : 'E', negative = latitude ? 'S' : 'W';
    if (strlen(hemisphere) != 1 ||
        (hemisphere[0] != positive && hemisphere[0] != negative)) return false;
    double raw;
    if (!decimal(text, &raw)) return false;
    int maximum = latitude ? 90 : 180;
    if (raw > maximum * 100.0) return false;
    int degrees = (int)(raw / 100.0);
    double minutes = raw - degrees * 100.0;
    if (minutes >= 60.0) return false;
    int32_t scaled = (int32_t)((degrees + minutes / 60.0) * 10000000.0 + 0.5);
    *result = hemisphere[0] == negative ? -scaled : scaled;
    return true;
}

/* Split once, preserving empty fields and rejecting truncation. Check all
 * fields, including optional ones, rather than silently shortening input. */
static bool fields_from_rmc(const char *line, char fields[10][24])
{
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || strlen(star) != 3 ||
        !isxdigit((unsigned char)star[1]) ||
        !isxdigit((unsigned char)star[2])) return false;
    unsigned checksum = 0;
    for (const char *p = line + 1; p < star; ++p) checksum ^= (unsigned char)*p;
    if (checksum != strtoul(star + 1, NULL, 16)) return false;

    unsigned field = 0;
    const char *start = line + 1;
    for (const char *p = start; p <= star; ++p) {
        if (p != star && *p != ',') {
            if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
            continue;
        }
        size_t length = (size_t)(p - start);
        if (length >= 24) return false;
        if (field < 10) memcpy(fields[field], start, length);
        ++field;
        start = p + 1;
    }
    return field >= 10 && strlen(fields[0]) == 5 &&
           isupper((unsigned char)fields[0][0]) &&
           isupper((unsigned char)fields[0][1]) &&
           strcmp(fields[0] + 2, "RMC") == 0 && strcmp(fields[2], "A") == 0;
}

/* Fixed-width decimal fields have already been bounded by the caller. */
static int digits(const char *text, unsigned count)
{
    int value = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!isdigit((unsigned char)text[i])) return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

static bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool parse_utc(const char *time, const char *date, int64_t *utc_ms)
{
    size_t length = strlen(time);
    if (strlen(date) != 6 || length < 6 ||
        (length > 6 && (time[6] != '.' || length == 7))) return false;
    int hour = digits(time, 2);
    int minute = digits(time + 2, 2);
    int second = digits(time + 4, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) return false;

    int milliseconds = 0, scale = 100;
    for (size_t i = 7; i < length; ++i) {
        int digit = digits(time + i, 1);
        if (digit < 0) return false;
        milliseconds += digit * scale;
        scale /= 10;
    }
    int day = digits(date, 2);
    int month = digits(date + 2, 2);
    int short_year = digits(date + 4, 2);
    if (short_year < 20 || month < 1 || month > 12) return false;
    int year = 2000 + short_year;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int days_in_month = month_days[month - 1] + (month == 2 && leap_year(year));
    if (day < 1 || day > days_in_month) return false;

    int64_t days = day - 1;
    for (int y = 1970; y < year; ++y) days += 365 + leap_year(y);
    for (int m = 1; m < month; ++m) {
        days += month_days[m - 1] + (m == 2 && leap_year(year));
    }
    *utc_ms = ((days * 24 + hour) * 3600 + minute * 60 + second) * 1000 + milliseconds;
    return true;
}

bool gps_rmc_parse(const char *line, gps_rmc_fix_t *fix)
{
    if (!fix) return false;
    *fix = (gps_rmc_fix_t){0};
    char fields[10][24] = {{0}};
    if (!fields_from_rmc(line, fields)) return false;
    gps_rmc_fix_t parsed = {0};
    if (fields[3][0] || fields[4][0] || fields[5][0] || fields[6][0]) {
        if (!coordinate(fields[3], fields[4], true, &parsed.latitude_e7) ||
            !coordinate(fields[5], fields[6], false, &parsed.longitude_e7)) return false;
        parsed.has_location = true;
    }
    if (fields[7][0]) {
        double knots;
        if (!decimal(fields[7], &knots)) return false;
        double speed = knots * 185.2; /* knots to km/h x100 */
        if (speed > UINT16_MAX) return false;
        parsed.speed_kph_x100 = (uint16_t)(speed + 0.5);
        parsed.has_speed = true;
    }
    parsed.has_utc = parse_utc(fields[1], fields[9], &parsed.utc_ms);
    *fix = parsed;
    return true;
}
