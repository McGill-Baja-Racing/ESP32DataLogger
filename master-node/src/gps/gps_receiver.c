#include "gps/gps_receiver.h"
#include "gps/rmc_parser.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"
#include "time/absolute_clock.h"

#define GPS_UART_NUM           UART_NUM_1
#define GPS_UART_BAUD_RATE     9600
#define GPS_UART_RX_GPIO       GPIO_NUM_33
#define GPS_UART_TX_GPIO       GPIO_NUM_32
#define GPS_UART_BUFFER_BYTES  1024
#define GPS_TASK_STACK_BYTES   4096
#define GPS_NMEA_LINE_MAX      128

static const char *TAG = "GpsReceiver";
static TaskHandle_t gps_task_handle;
static gps_sample_handler_t on_sample;

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
    bool overflow = false;
    uint8_t byte;
    ESP_LOGI(TAG, "UART ready: uart=%d baud=%d rx_gpio=%d tx_gpio=%d",
             GPS_UART_NUM, GPS_UART_BAUD_RATE, GPS_UART_RX_GPIO, GPS_UART_TX_GPIO);
    while (true) {
        if (uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000)) <= 0) continue;
        if (byte == '\n') {
            if (length && line[length - 1] == '\r') --length;
            line[length] = '\0';
            if (overflow) { length = 0; overflow = false; continue; }
            gps_rmc_fix_t fix;
            if (!gps_rmc_parse(line, &fix)) { length = 0; continue; }
            if (fix.has_utc) absolute_clock_observe_utc(fix.utc_ms);
            if (fix.has_location && fix.has_speed) {
                uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
                emit_sample(CAN_ID_GPS_SPEED, fix.speed_kph_x100, timestamp_ms);
                emit_sample(CAN_ID_GPS_LATITUDE, fix.latitude_e7, timestamp_ms);
                emit_sample(CAN_ID_GPS_LONGITUDE, fix.longitude_e7, timestamp_ms);
                ESP_LOGI(TAG, "Fix: lat_e7=%" PRId32 " lon_e7=%" PRId32
                         " speed_kph_x100=%u", fix.latitude_e7,
                         fix.longitude_e7, fix.speed_kph_x100);
            }
            length = 0;
        } else if (byte == '\0') {
            overflow = true;
        } else {
            if (length < sizeof(line) - 1) line[length++] = (char)byte;
            else overflow = true;
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
