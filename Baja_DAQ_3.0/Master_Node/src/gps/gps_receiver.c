#include "gps/gps_receiver.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"

#define GPS_UART_NUM           UART_NUM_1
#define GPS_UART_BAUD_RATE     9600
#define GPS_UART_RX_GPIO       GPIO_NUM_33
#define GPS_UART_TX_GPIO       GPIO_NUM_32
#define GPS_UART_BUFFER_BYTES  1024
#define GPS_TASK_STACK_BYTES   4096
#define GPS_NMEA_LINE_MAX      128

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint16_t speed_kph_x100;
    bool valid;
    bool has_location;
} gps_fix_t;

static const char *TAG = "GpsReceiver";
static TaskHandle_t gps_task_handle;
static gps_sample_handler_t on_sample;

static bool checksum_valid(const char *line)
{
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || !isxdigit((unsigned char)star[1]) ||
        !isxdigit((unsigned char)star[2])) return false;
    uint8_t checksum = 0;
    for (const char *cursor = line + 1; cursor < star; cursor++) {
        checksum ^= (uint8_t)*cursor;
    }
    char expected[3] = {star[1], star[2], '\0'};
    return checksum == (uint8_t)strtoul(expected, NULL, 16);
}

static bool field_copy(const char *line, size_t wanted, char *output,
                       size_t output_size)
{
    if (!line || !output || output_size == 0) return false;
    output[0] = '\0';
    const char *cursor = line + (line[0] == '$');
    const char *start = cursor;
    size_t field = 0;
    while (*cursor && *cursor != '\r' && *cursor != '\n') {
        if (*cursor == ',' || *cursor == '*') {
            if (field == wanted) {
                size_t length = (size_t)(cursor - start);
                if (length >= output_size) length = output_size - 1;
                memcpy(output, start, length);
                output[length] = '\0';
                return length > 0;
            }
            if (*cursor == '*') return false;
            field++;
            start = cursor + 1;
        }
        cursor++;
    }
    return false;
}

static int32_t decimal_scaled(const char *text, int32_t scale)
{
    char *end;
    double value = strtod(text, &end);
    if (!text[0] || end == text) return 0;
    double scaled = value * scale;
    return (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

static bool parse_coordinate(const char *text, const char *hemisphere,
                             int32_t *coordinate_e7)
{
    char *end;
    double raw = strtod(text, &end);
    if (!text[0] || end == text || raw <= 0.0 || !hemisphere[0]) return false;
    int degrees = (int)(raw / 100.0);
    double decimal = degrees + (raw - degrees * 100.0) / 60.0;
    char direction = (char)toupper((unsigned char)hemisphere[0]);
    if (direction == 'S' || direction == 'W') decimal = -decimal;
    else if (direction != 'N' && direction != 'E') return false;
    double scaled = decimal * 10000000.0;
    *coordinate_e7 = (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    return true;
}

static bool parse_rmc(const char *line, gps_fix_t *fix)
{
    if (!checksum_valid(line)) return false;
    char type[8], status[3], latitude[20], north_south[3];
    char longitude[20], east_west[3], speed_knots[16];
    if (!field_copy(line, 0, type, sizeof(type)) || strlen(type) < 5 ||
        strcmp(type + strlen(type) - 3, "RMC") != 0) return false;
    fix->valid = field_copy(line, 2, status, sizeof(status)) &&
                 toupper((unsigned char)status[0]) == 'A';
    fix->has_location =
        field_copy(line, 3, latitude, sizeof(latitude)) &&
        field_copy(line, 4, north_south, sizeof(north_south)) &&
        field_copy(line, 5, longitude, sizeof(longitude)) &&
        field_copy(line, 6, east_west, sizeof(east_west)) &&
        parse_coordinate(latitude, north_south, &fix->latitude_e7) &&
        parse_coordinate(longitude, east_west, &fix->longitude_e7);
    if (field_copy(line, 7, speed_knots, sizeof(speed_knots))) {
        int32_t knots_x100 = decimal_scaled(speed_knots, 100);
        fix->speed_kph_x100 = (uint16_t)((knots_x100 * 1852 + 500) / 1000);
    }
    return true;
}

static void emit_sample(uint32_t id, int32_t value, uint32_t timestamp_ms)
{
    if (!on_sample) return;
    can_message_t sample = {
        .id = id,
        .dlc = 8,
        .data = ((uint64_t)timestamp_ms << 32) | (uint32_t)value,
    };
    on_sample(&sample);
}

static void gps_task(void *argument)
{
    (void)argument;
    char line[GPS_NMEA_LINE_MAX] = {0};
    size_t length = 0;
    uint8_t byte;
    ESP_LOGI(TAG, "UART ready: uart=%d baud=%d rx_gpio=%d tx_gpio=%d",
             GPS_UART_NUM, GPS_UART_BAUD_RATE, GPS_UART_RX_GPIO, GPS_UART_TX_GPIO);
    while (true) {
        if (uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000)) <= 0) continue;
        if (byte == '\n') {
            line[length] = '\0';
            gps_fix_t fix = {0};
            if (length && parse_rmc(line, &fix) && fix.valid && fix.has_location) {
                uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
                emit_sample(CAN_ID_GPS_SPEED, fix.speed_kph_x100, timestamp_ms);
                emit_sample(CAN_ID_GPS_LATITUDE, fix.latitude_e7, timestamp_ms);
                emit_sample(CAN_ID_GPS_LONGITUDE, fix.longitude_e7, timestamp_ms);
                ESP_LOGI(TAG, "Fix: lat_e7=%" PRId32 " lon_e7=%" PRId32
                         " speed_kph_x100=%u", fix.latitude_e7,
                         fix.longitude_e7, fix.speed_kph_x100);
            }
            length = 0;
        } else if (byte != '\r') {
            if (length < sizeof(line) - 1) line[length++] = (char)byte;
            else length = 0;
        }
    }
}

esp_err_t gps_receiver_start(gps_sample_handler_t sample_handler)
{
    if (!sample_handler) return ESP_ERR_INVALID_ARG;
    if (gps_task_handle) return ESP_ERR_INVALID_STATE;
    on_sample = sample_handler;
    uart_config_t config = {
        .baud_rate = GPS_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t error = uart_driver_install(GPS_UART_NUM, GPS_UART_BUFFER_BYTES,
                                          0, 0, NULL, 0);
    if (error == ESP_OK) error = uart_param_config(GPS_UART_NUM, &config);
    if (error == ESP_OK) {
        error = uart_set_pin(GPS_UART_NUM, GPS_UART_TX_GPIO, GPS_UART_RX_GPIO,
                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (error != ESP_OK) {
        uart_driver_delete(GPS_UART_NUM);
        on_sample = NULL;
        return error;
    }
    if (xTaskCreate(gps_task, "gps_receiver", GPS_TASK_STACK_BYTES, NULL, 6,
                    &gps_task_handle) != pdPASS) {
        uart_driver_delete(GPS_UART_NUM);
        on_sample = NULL;
        gps_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
