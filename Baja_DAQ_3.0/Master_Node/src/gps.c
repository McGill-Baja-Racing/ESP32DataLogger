#include "gps.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "master_control.h"

static const char *TAG = "gps";

#define GPS_UART_NUM UART_NUM_1
#define GPS_UART_BAUD_RATE 9600
#define GPS_UART_RX_GPIO GPIO_NUM_33
#define GPS_UART_TX_GPIO GPIO_NUM_32
#define GPS_UART_BUFFER_BYTES 1024
#define GPS_TASK_STACK_BYTES 4096
#define GPS_NMEA_LINE_MAX 128
#define GPS_MAX_SIGNALS 8

static TaskHandle_t s_gps_task_handle;
static gps_signal_config_t s_gps_signals[GPS_MAX_SIGNALS];
static uint32_t s_gps_last_emit_ms[GPS_MAX_SIGNALS];
static size_t s_gps_signal_count;
static volatile bool s_gps_logging_enabled;
static portMUX_TYPE s_gps_config_lock = portMUX_INITIALIZER_UNLOCKED;

void gps_configure_signals(const gps_signal_config_t *signals, size_t signal_count)
{
    if (signal_count > GPS_MAX_SIGNALS) {
        signal_count = GPS_MAX_SIGNALS;
    }

    portENTER_CRITICAL(&s_gps_config_lock);
    memset(s_gps_signals, 0, sizeof(s_gps_signals));
    memset(s_gps_last_emit_ms, 0, sizeof(s_gps_last_emit_ms));
    for (size_t i = 0; i < signal_count; i++) {
        s_gps_signals[i] = signals[i];
        if (s_gps_signals[i].sample_rate_hz == 0) {
            s_gps_signals[i].sample_rate_hz = 1;
        }
    }
    s_gps_signal_count = signal_count;
    portEXIT_CRITICAL(&s_gps_config_lock);
}

void gps_set_logging_enabled(bool enabled)
{
    s_gps_logging_enabled = enabled;
}

static bool gps_signal_value(const gps_signal_config_t *signal,
                             const gps_fix_t *fix,
                             uint32_t *out_value)
{
    if (!signal || !fix || !out_value || !fix->valid || !fix->has_location) {
        return false;
    }

    if (strcasecmp(signal->function, "gps_speed") == 0) {
        *out_value = fix->speed_kph_x100;
        return true;
    }

    if (strcasecmp(signal->function, "gps_latitude") == 0) {
        *out_value = (uint32_t)fix->latitude_e7;
        return true;
    }

    if (strcasecmp(signal->function, "gps_longitude") == 0) {
        *out_value = (uint32_t)fix->longitude_e7;
        return true;
    }

    return false;
}

static void gps_emit_due_samples(const gps_fix_t *fix)
{
    gps_signal_config_t signals[GPS_MAX_SIGNALS];
    uint32_t last_emit_ms[GPS_MAX_SIGNALS];
    size_t signal_count = 0;
    size_t emitted_count = 0;

    portENTER_CRITICAL(&s_gps_config_lock);
    signal_count = s_gps_signal_count;
    memcpy(signals, s_gps_signals, sizeof(signals));
    memcpy(last_emit_ms, s_gps_last_emit_ms, sizeof(last_emit_ms));
    portEXIT_CRITICAL(&s_gps_config_lock);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (size_t i = 0; i < signal_count; i++) {
        if (!signals[i].enabled || signals[i].can_id == 0) {
            continue;
        }

        uint16_t rate_hz = signals[i].sample_rate_hz ? signals[i].sample_rate_hz : 1;
        uint32_t period_ms = 1000u / rate_hz;
        if (period_ms == 0) {
            period_ms = 1;
        }

        if (last_emit_ms[i] != 0 && (uint32_t)(now_ms - last_emit_ms[i]) < period_ms) {
            continue;
        }

        uint32_t value = 0;
        if (!gps_signal_value(&signals[i], fix, &value)) {
            continue;
        }

        bool preview = signals[i].preview_enabled;
        if (!s_gps_logging_enabled && !preview) {
            continue;
        }

        if (master_submit_local_sample(signals[i].can_id, value, now_ms, preview)) {
            emitted_count++;
        }

        portENTER_CRITICAL(&s_gps_config_lock);
        if (i < s_gps_signal_count && s_gps_signals[i].can_id == signals[i].can_id) {
            s_gps_last_emit_ms[i] = now_ms;
        }
        portEXIT_CRITICAL(&s_gps_config_lock);
    }

    if (emitted_count > 0 && s_gps_logging_enabled) {
        ESP_LOGI(TAG,
                 "GPS logged %u sample(s): speed_kph_x100=%" PRIu16
                 " lat_e7=%" PRId32 " lon_e7=%" PRId32,
                 (unsigned)emitted_count,
                 fix->speed_kph_x100,
                 fix->latitude_e7,
                 fix->longitude_e7);
    }
}

static bool nmea_checksum_valid(const char *line)
{
    if (!line || line[0] != '$') {
        return false;
    }

    const char *star = strchr(line, '*');
    if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) {
        return false;
    }

    uint8_t checksum = 0;
    for (const char *p = line + 1; p < star; p++) {
        checksum ^= (uint8_t)*p;
    }

    char expected_text[3] = {star[1], star[2], '\0'};
    uint8_t expected = (uint8_t)strtoul(expected_text, NULL, 16);
    return checksum == expected;
}

static bool nmea_field_copy(const char *line, size_t wanted, char *out, size_t out_size)
{
    if (!line || !out || out_size == 0) {
        return false;
    }

    out[0] = '\0';

    const char *p = line;
    if (*p == '$') {
        p++;
    }

    size_t field = 0;
    const char *start = p;
    while (*p && *p != '\r' && *p != '\n') {
        if (*p == ',' || *p == '*') {
            if (field == wanted) {
                size_t len = (size_t)(p - start);
                if (len >= out_size) {
                    len = out_size - 1;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return len > 0;
            }

            if (*p == '*') {
                return false;
            }

            field++;
            start = p + 1;
        }
        p++;
    }

    if (field == wanted) {
        size_t len = (size_t)(p - start);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, start, len);
        out[len] = '\0';
        return len > 0;
    }

    return false;
}

static int32_t decimal_scaled_or_zero(const char *text, int scale)
{
    if (!text || text[0] == '\0') {
        return 0;
    }

    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text) {
        return 0;
    }

    double scaled = value * (double)scale;
    return (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

static bool parse_lat_lon_e7(const char *value_text, const char *hemisphere, int32_t *out_e7)
{
    if (!value_text || !hemisphere || !out_e7 || value_text[0] == '\0' ||
        hemisphere[0] == '\0') {
        return false;
    }

    char *end = NULL;
    double raw = strtod(value_text, &end);
    if (end == value_text || raw <= 0.0) {
        return false;
    }

    int degrees = (int)(raw / 100.0);
    double minutes = raw - ((double)degrees * 100.0);
    double decimal = (double)degrees + (minutes / 60.0);

    char hemi = (char)toupper((unsigned char)hemisphere[0]);
    if (hemi == 'S' || hemi == 'W') {
        decimal = -decimal;
    } else if (hemi != 'N' && hemi != 'E') {
        return false;
    }

    double scaled = decimal * 10000000.0;
    *out_e7 = (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    return true;
}

static void parse_gga_sentence(const char *line, gps_fix_t *fix)
{
    char utc[16];
    char lat[20];
    char ns[3];
    char lon[20];
    char ew[3];
    char quality[4];
    char satellites[4];
    char hdop[12];
    char altitude[16];

    if (nmea_field_copy(line, 1, utc, sizeof(utc))) {
        snprintf(fix->utc_time, sizeof(fix->utc_time), "%s", utc);
    }

    if (nmea_field_copy(line, 2, lat, sizeof(lat)) &&
        nmea_field_copy(line, 3, ns, sizeof(ns)) &&
        nmea_field_copy(line, 4, lon, sizeof(lon)) &&
        nmea_field_copy(line, 5, ew, sizeof(ew))) {
        int32_t lat_e7 = 0;
        int32_t lon_e7 = 0;
        if (parse_lat_lon_e7(lat, ns, &lat_e7) &&
            parse_lat_lon_e7(lon, ew, &lon_e7)) {
            fix->latitude_e7 = lat_e7;
            fix->longitude_e7 = lon_e7;
            fix->has_location = true;
        }
    }

    if (nmea_field_copy(line, 6, quality, sizeof(quality))) {
        fix->fix_quality = (uint8_t)atoi(quality);
        fix->valid = fix->fix_quality > 0;
    }

    if (nmea_field_copy(line, 7, satellites, sizeof(satellites))) {
        fix->satellites = (uint8_t)atoi(satellites);
    }

    if (nmea_field_copy(line, 8, hdop, sizeof(hdop))) {
        fix->hdop_x100 = (uint16_t)decimal_scaled_or_zero(hdop, 100);
    }

    if (nmea_field_copy(line, 9, altitude, sizeof(altitude))) {
        fix->altitude_cm = decimal_scaled_or_zero(altitude, 100);
    }
}

static void parse_rmc_sentence(const char *line, gps_fix_t *fix)
{
    char utc[16];
    char status[3];
    char lat[20];
    char ns[3];
    char lon[20];
    char ew[3];
    char speed_knots[16];
    char course[16];
    char date[8];

    if (nmea_field_copy(line, 1, utc, sizeof(utc))) {
        snprintf(fix->utc_time, sizeof(fix->utc_time), "%s", utc);
    }

    if (nmea_field_copy(line, 2, status, sizeof(status))) {
        fix->valid = toupper((unsigned char)status[0]) == 'A';
    }

    if (nmea_field_copy(line, 3, lat, sizeof(lat)) &&
        nmea_field_copy(line, 4, ns, sizeof(ns)) &&
        nmea_field_copy(line, 5, lon, sizeof(lon)) &&
        nmea_field_copy(line, 6, ew, sizeof(ew))) {
        int32_t lat_e7 = 0;
        int32_t lon_e7 = 0;
        if (parse_lat_lon_e7(lat, ns, &lat_e7) &&
            parse_lat_lon_e7(lon, ew, &lon_e7)) {
            fix->latitude_e7 = lat_e7;
            fix->longitude_e7 = lon_e7;
            fix->has_location = true;
        }
    }

    if (nmea_field_copy(line, 7, speed_knots, sizeof(speed_knots))) {
        int32_t knots_x100 = decimal_scaled_or_zero(speed_knots, 100);
        fix->speed_kph_x100 = (uint16_t)((knots_x100 * 1852 + 500) / 1000);
    }

    if (nmea_field_copy(line, 8, course, sizeof(course))) {
        fix->course_deg_x100 = (uint16_t)decimal_scaled_or_zero(course, 100);
    }

    if (nmea_field_copy(line, 9, date, sizeof(date))) {
        snprintf(fix->utc_date, sizeof(fix->utc_date), "%s", date);
    }
}

static bool parse_nmea_sentence(const char *line, gps_fix_t *fix)
{
    if (!line || !fix || !nmea_checksum_valid(line)) {
        return false;
    }

    char type[8];
    if (!nmea_field_copy(line, 0, type, sizeof(type))) {
        return false;
    }

    if (strlen(type) < 5) {
        return false;
    }

    const char *sentence = type + strlen(type) - 3;
    if (strcmp(sentence, "GGA") == 0) {
        parse_gga_sentence(line, fix);
    } else if (strcmp(sentence, "RMC") == 0) {
        parse_rmc_sentence(line, fix);
    } else {
        return false;
    }

    fix->timestamp_us = esp_timer_get_time();
    return true;
}

static void gps_task(void *arg)
{
    (void)arg;

    gps_fix_t fix = {};
    char line[GPS_NMEA_LINE_MAX] = {};
    size_t line_len = 0;
    uint8_t byte = 0;

    ESP_LOGI(TAG,
             "GPS UART ready: uart=%d baud=%d rx_gpio=%d tx_gpio=%d",
             GPS_UART_NUM,
             GPS_UART_BAUD_RATE,
             GPS_UART_RX_GPIO,
             GPS_UART_TX_GPIO);

    while (1) {
        int read = uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000));
        if (read <= 0) {
            continue;
        }

        if (byte == '\n') {
            line[line_len] = '\0';
            if (line_len > 0 && parse_nmea_sentence(line, &fix)) {
                if (strstr(line, "RMC") != NULL) {
                    gps_emit_due_samples(&fix);
                }
                if (fix.valid && fix.has_location) {
                    ESP_LOGI(TAG,
                             "GPS fix lat_e7=%" PRId32 " lon_e7=%" PRId32
                             " sats=%u hdop_x100=%u",
                             fix.latitude_e7,
                             fix.longitude_e7,
                             fix.satellites,
                             fix.hdop_x100);
                }
            }
            line_len = 0;
            continue;
        }

        if (byte == '\r') {
            continue;
        }

        if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
        } else {
            line_len = 0;
        }
    }
}

esp_err_t gps_start(void)
{
    if (s_gps_task_handle) {
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = GPS_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(GPS_UART_NUM,
                                        GPS_UART_BUFFER_BYTES,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install GPS UART driver: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(GPS_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPS UART: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(GPS_UART_NUM,
                       GPS_UART_TX_GPIO,
                       GPS_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPS UART pins: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t task_ok = xTaskCreate(gps_task,
                                     "gps",
                                     GPS_TASK_STACK_BYTES,
                                     NULL,
                                     6,
                                     &s_gps_task_handle);
    if (task_ok != pdPASS) {
        s_gps_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create GPS task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
