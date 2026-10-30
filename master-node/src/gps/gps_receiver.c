#include "gps/gps_receiver.h"
#include "gps/rmc_parser.h"
#include "gps/ubx_config.h"

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
/* After each measurement (every 200 ms at 5 Hz) the GPS sends a burst of
 * NMEA sentences, then goes quiet. A byte after at least this much silence
 * starts a new burst. RMC alone takes about 75 ms at 9600 baud, leaving about
 * 125 ms of silence. */
#define GPS_BURST_GAP_US       50000
/* How long after its measurement time the GPS begins sending that
 * measurement's burst. 0 until measured on the real receiver (can be overridden
 * with -D GPS_UTC_OUTPUT_DELAY_US=...); see docs/ABSOLUTE_TIME.md. */
#ifndef GPS_UTC_OUTPUT_DELAY_US
#define GPS_UTC_OUTPUT_DELAY_US 0
#endif
/* The NEO-6M answers within tens of ms. Without a receiver, setup gives up
 * after 5 commands x 2 attempts x 250 ms = 2.5 s. */
#define GPS_ACK_TIMEOUT_US     250000
#define GPS_COMMAND_ATTEMPTS   2
/* Fixes arrive five times a second; log only one per this period. */
#define GPS_FIX_LOG_PERIOD_US  1000000

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

static bool starts_new_burst(int64_t received_us, int64_t previous_byte_us)
{
    bool first_byte_ever = previous_byte_us == 0;
    return first_byte_ever || received_us - previous_byte_us >= GPS_BURST_GAP_US;
}

/* Wait for the receiver to acknowledge a command, skipping the NMEA text it
 * keeps sending meanwhile. */
static bool wait_for_ack(const uint8_t *ack, size_t length)
{
    size_t matched = 0;
    int64_t deadline_us = esp_timer_get_time() + GPS_ACK_TIMEOUT_US;
    uint8_t byte;
    while (esp_timer_get_time() < deadline_us) {
        if (uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(10)) <= 0) continue;
        if (byte == ack[matched]) {
            if (++matched == length) return true;
        } else {
            matched = byte == ack[0];
        }
    }
    return false;
}

/* Ask the receiver for RMC only, five times a second. The setting lives in the
 * receiver's RAM and is not saved, so it is sent on every boot. Without
 * acknowledgements the receiver keeps its 1 Hz default and everything else
 * still works. */
static void configure_update_rate(void)
{
    uint8_t command[UBX_FRAME_MAX], ack[UBX_FRAME_MAX];
    unsigned acknowledged = 0;
    for (unsigned i = 0; i < UBX_RATE_COMMAND_COUNT; ++i) {
        /* The last command raises the rate. With any extra sentence still on,
         * 5 Hz bursts would run into each other, so stay at 1 Hz instead. */
        if (i == UBX_RATE_COMMAND_COUNT - 1 && acknowledged < i) break;
        size_t length = ubx_rate_command(i, command);
        size_t ack_length = ubx_expected_ack(command, ack);
        for (unsigned attempt = 0; attempt < GPS_COMMAND_ATTEMPTS; ++attempt) {
            uart_write_bytes(GPS_UART_NUM, command, length);
            if (wait_for_ack(ack, ack_length)) { ++acknowledged; break; }
        }
    }
    if (acknowledged == UBX_RATE_COMMAND_COUNT) {
        ESP_LOGI(TAG, "Receiver set to RMC every %d ms", UBX_MEASUREMENT_PERIOD_MS);
    } else {
        ESP_LOGW(TAG, "Receiver acknowledged %u of %d rate commands; staying at "
                 "its 1 Hz default", acknowledged, UBX_RATE_COMMAND_COUNT);
    }
    /* Drop the acknowledgements and any half-read sentence, so the first burst
     * the reader times is a real one. */
    uart_flush_input(GPS_UART_NUM);
}

static void read_sentences(void)
{
    char line[GPS_NMEA_LINE_MAX] = {0};
    size_t length = 0;
    bool overflow = false;
    uint8_t byte;
    /* When the current burst's first byte arrived. Reset to 0 once used, so
     * only the first RMC sentence of each burst sets the clock. */
    int64_t burst_start_us = 0;
    int64_t previous_byte_us = 0;
    int64_t last_fix_log_us = -GPS_FIX_LOG_PERIOD_US;
    while (true) {
        if (uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000)) <= 0) continue;
        int64_t received_us = esp_timer_get_time();
        if (starts_new_burst(received_us, previous_byte_us)) burst_start_us = received_us;
        previous_byte_us = received_us;
        if (byte == '\n') {
            if (length && line[length - 1] == '\r') --length;
            line[length] = '\0';
            if (overflow) { length = 0; overflow = false; continue; }
            gps_rmc_fix_t fix;
            if (!gps_rmc_parse(line, &fix)) { length = 0; continue; }
            /* Pair the GPS time with when the burst started, not with now:
             * at 9600 baud this sentence alone took about 75 ms to arrive. */
            if (fix.has_utc && burst_start_us) {
                int64_t measured_at_us = burst_start_us - GPS_UTC_OUTPUT_DELAY_US;
                absolute_clock_observe_utc(fix.utc_ms, measured_at_us);
                burst_start_us = 0;
            }
            if (fix.has_location && fix.has_speed) {
                uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
                emit_sample(CAN_ID_GPS_SPEED, fix.speed_kph_x100, timestamp_ms);
                emit_sample(CAN_ID_GPS_LATITUDE, fix.latitude_e7, timestamp_ms);
                emit_sample(CAN_ID_GPS_LONGITUDE, fix.longitude_e7, timestamp_ms);
                if (received_us - last_fix_log_us >= GPS_FIX_LOG_PERIOD_US) {
                    last_fix_log_us = received_us;
                    ESP_LOGI(TAG, "Fix: lat_e7=%" PRId32 " lon_e7=%" PRId32
                             " speed_kph_x100=%u", fix.latitude_e7,
                             fix.longitude_e7, fix.speed_kph_x100);
                }
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

static void gps_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "UART ready: uart=%d baud=%d rx_gpio=%d tx_gpio=%d",
             GPS_UART_NUM, GPS_UART_BAUD_RATE, GPS_UART_RX_GPIO, GPS_UART_TX_GPIO);
    configure_update_rate();
    read_sentences();
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
    /* Hand over each byte as soon as it arrives. By default the driver waits
     * for its buffer to fill or the line to go quiet, which would delay the
     * burst start timestamp. */
    if (error == ESP_OK) error = uart_set_rx_full_threshold(GPS_UART_NUM, 1);
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
