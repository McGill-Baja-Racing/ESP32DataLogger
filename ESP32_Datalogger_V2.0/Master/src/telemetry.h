#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    bool has_location;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_cm;
    uint16_t speed_kph_x100;
    uint16_t course_deg_x100;
    uint16_t hdop_x100;
    uint8_t satellites;
    uint8_t fix_quality;
    int64_t timestamp_us;
    char utc_time[16];
    char utc_date[8];
} telemetry_gps_fix_t;

esp_err_t telemetry_start(void);
void telemetry_submit_can_frame(uint32_t id, uint8_t dlc, uint64_t data);
void telemetry_submit_gps_fix(const telemetry_gps_fix_t *fix);
