#include "web/web_server.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app/app_control.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logger/data_logger.h"
#include "live/live_data.h"
#include "protocol/app_protocol.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#define AP_SSID "BajaDAQ"
#define SD_ROOT "/sdcard"
#define DOWNLOAD_CHUNK_SIZE 4096

typedef struct {
    char name[DATA_LOGGER_FILENAME_MAX + 1];
    off_t size;
} log_entry_t;

static const char *TAG = "WebServer";
static httpd_handle_t server;
static SemaphoreHandle_t download_mutex;

extern const uint8_t web_index_html_start[];
extern const uint8_t web_index_html_end[];

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int alphabetical_descending(const void *left, const void *right)
{
    const log_entry_t *a = left;
    const log_entry_t *b = right;
    return strcmp(b->name, a->name);
}

static log_entry_t *load_completed_logs(size_t *count)
{
    *count = 0;
    errno = 0;
    DIR *directory = opendir(SD_ROOT);
    if (!directory) return NULL;
    size_t capacity = 0;
    log_entry_t *entries = NULL;
    const char *active = data_logger_state() == LOGGER_IDLE
                       ? NULL : base_name(data_logger_path());
    struct dirent *item;
    while ((item = readdir(directory)) != NULL) {
        if (!data_logger_valid_filename(item->d_name) ||
            (active && strcmp(item->d_name, active) == 0)) {
            continue;
        }
        char path[96];
        struct stat info;
        snprintf(path, sizeof(path), SD_ROOT "/%s", item->d_name);
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            log_entry_t *grown = realloc(entries, next * sizeof(*entries));
            if (!grown) {
                free(entries);
                closedir(directory);
                errno = ENOMEM;
                return NULL;
            }
            entries = grown;
            capacity = next;
        }
        snprintf(entries[*count].name, sizeof(entries[*count].name), "%s",
                 item->d_name);
        entries[*count].size = info.st_size;
        (*count)++;
    }
    closedir(directory);
    qsort(entries, *count, sizeof(*entries), alphabetical_descending);
    errno = 0;
    return entries;
}

static esp_err_t send_json_error(httpd_req_t *request, const char *status,
                                 const char *message)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)web_index_html_start,
                           web_index_html_end - web_index_html_start);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    app_status_t status;
    char body[512];
    app_control_get_status(&status);
    snprintf(body, sizeof(body),
             "{\"logger_state\":\"%s\",\"current_file\":\"%s\","
             "\"can_drops\":%" PRIu32 ",\"log_drops\":%" PRIu32 ","
             "\"can_rx_errors\":%" PRIu32 ",\"can_tx_errors\":%" PRIu32 ","
             "\"can_errors_available\":%s,"
             "\"live_enabled\":%s,"
             "\"nodes\":{\"1\":\"%s\",\"3\":\"%s\",\"4\":\"%s\",\"5\":\"%s\","
             "\"6\":\"%s\"}}",
             data_logger_state_name(), base_name(status.current_file),
             status.can_drops, status.log_drops,
             status.can_rx_errors, status.can_tx_errors,
             status.can_errors_available ? "true" : "false",
             status.live_enabled ? "true" : "false", status.node_1, status.node_3, status.node_4,
             status.node_5, status.node_6);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t command_handler(httpd_req_t *request)
{
    bool starting = strncmp(request->uri, "/api/logging/start", 18) == 0;
    char filename[DATA_LOGGER_FILENAME_MAX + 1];
    const char *selected_name = NULL;
    if (starting) {
        char query[96];
        esp_err_t query_result = httpd_req_get_url_query_str(
            request, query, sizeof(query));
        if (query_result == ESP_OK) {
            if (httpd_query_key_value(query, "name", filename,
                                      sizeof(filename)) != ESP_OK ||
                !data_logger_valid_filename(filename)) {
                return send_json_error(
                    request, "400 Bad Request",
                    "Use 1-40 letters, numbers, underscores or hyphens, followed by .bin");
            }
            selected_name = filename;
        } else if (query_result != ESP_ERR_NOT_FOUND) {
            return send_json_error(request, "400 Bad Request",
                                   "Log filename is too long");
        }
    }
    app_control_result_t result = starting
        ? app_control_start_logging(selected_name) : app_control_stop_logging();
    if (result == APP_CONTROL_CONFLICT) {
        return send_json_error(request, "409 Conflict",
                               "Command is not valid in the current logger state");
    }
    if (result == APP_CONTROL_INVALID_FILENAME) {
        return send_json_error(
            request, "400 Bad Request",
            "Use 1-40 letters, numbers, underscores or hyphens, followed by .bin");
    }
    if (result == APP_CONTROL_FILENAME_EXISTS) {
        return send_json_error(request, "409 Conflict",
                               "A log with this filename already exists");
    }
    if (result != APP_CONTROL_OK) {
        return send_json_error(request, "500 Internal Server Error",
                               "Logger command failed");
    }
    return status_handler(request);
}

static esp_err_t logs_handler(httpd_req_t *request)
{
    size_t count;
    log_entry_t *entries = load_completed_logs(&count);
    if (!entries && count == 0 && errno != 0) {
        return send_json_error(request, "500 Internal Server Error",
                               "Unable to read the SD card");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, "[", 1);
    for (size_t i = 0; i < count; i++) {
        char item[128];
        int length = snprintf(item, sizeof(item), "%s{\"name\":\"%s\","
                              "\"size_bytes\":%" PRIu64 "}",
                              i ? "," : "", entries[i].name,
                              (uint64_t)entries[i].size);
        if (httpd_resp_send_chunk(request, item, length) != ESP_OK) break;
    }
    free(entries);
    httpd_resp_send_chunk(request, "]", 1);
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t rename_log_handler(httpd_req_t *request)
{
    char query[128];
    char old_name[DATA_LOGGER_FILENAME_MAX + 1];
    char new_name[DATA_LOGGER_FILENAME_MAX + 1];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", old_name, sizeof(old_name)) != ESP_OK ||
        httpd_query_key_value(query, "new_name", new_name, sizeof(new_name)) != ESP_OK ||
        !data_logger_valid_filename(old_name) ||
        !data_logger_valid_filename(new_name)) {
        return send_json_error(
            request, "400 Bad Request",
            "Use 1-40 letters, numbers, underscores or hyphens, followed by .bin");
    }
    if (xSemaphoreTake(download_mutex, 0) != pdTRUE) {
        return send_json_error(request, "503 Service Unavailable",
                               "Another log operation is already active");
    }
    data_logger_rename_result_t result = data_logger_rename(old_name, new_name);
    xSemaphoreGive(download_mutex);
    if (result == DATA_LOGGER_RENAME_ACTIVE) {
        return send_json_error(request, "409 Conflict",
                               "The active log cannot be renamed");
    }
    if (result == DATA_LOGGER_RENAME_NOT_FOUND) {
        return send_json_error(request, "404 Not Found", "Log file not found");
    }
    if (result == DATA_LOGGER_RENAME_EXISTS) {
        return send_json_error(request, "409 Conflict",
                               "A log with the new filename already exists");
    }
    if (result == DATA_LOGGER_RENAME_INVALID_NAME) {
        return send_json_error(request, "400 Bad Request", "Invalid log filename");
    }
    if (result != DATA_LOGGER_RENAME_OK) {
        return send_json_error(request, "500 Internal Server Error",
                               "Unable to rename the log file");
    }
    char body[80];
    snprintf(body, sizeof(body), "{\"name\":\"%s\"}", new_name);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static const live_signal_metadata_t *metadata_for(uint32_t can_id)
{
    size_t count;
    const live_signal_metadata_t *signals = live_data_signals(&count);
    for (size_t i = 0; i < count; i++) {
        if (signals[i].can_id == can_id) return &signals[i];
    }
    return NULL;
}

static esp_err_t stream_binary(httpd_req_t *request, FILE *file)
{
    char *buffer = malloc(DOWNLOAD_CHUNK_SIZE);
    if (!buffer) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    size_t count;
    while ((count = fread(buffer, 1, DOWNLOAD_CHUNK_SIZE, file)) > 0) {
        if (httpd_resp_send_chunk(request, buffer, count) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    if (ferror(file)) result = ESP_FAIL;
    free(buffer);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

static int64_t read_utc_ms(FILE *utc_file)
{
    int64_t utc_ms = 0;
    if (!utc_file || fread(&utc_ms, sizeof(utc_ms), 1, utc_file) != 1) return 0;
    return utc_ms > 0 ? utc_ms : 0;
}

static void format_absolute_time(int64_t utc_ms, char *absolute_time, size_t size)
{
    absolute_time[0] = '\0';
    if (utc_ms <= 0) return;
    time_t seconds = (time_t)(utc_ms / 1000);
    struct tm utc;
    char date[24];
    if (gmtime_r(&seconds, &utc) &&
        strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc)) {
        snprintf(absolute_time, size, "%s.%03uZ", date,
                 (unsigned)(utc_ms % 1000));
    }
}

static bool sample_at_or_before(uint32_t timestamp, uint32_t sample_timestamp)
{
    return (uint32_t)(timestamp - sample_timestamp) < 0x80000000U;
}

/* Emit one row per engine sample with a recent bearing sample. Lower-rate GPS
 * values are held between fixes. Front-brake pressure must be recent to avoid
 * presenting a stale pressure after its sensor goes offline. */
static esp_err_t stream_paired_csv(httpd_req_t *request, FILE *file,
                                   FILE *utc_file)
{
    static const char header[] =
        "Relative time,Absolute time,Brake pressure,Bearing RPM,Engine RPM,"
        "GPS latitude,GPS longitude,GPS Speed\r\n";
    if (httpd_resp_send_chunk(request, header, sizeof(header) - 1) != ESP_OK) {
        return ESP_FAIL;
    }
    uint64_t record[2];
    uint32_t bearing_timestamp = 0, brake_timestamp = 0;
    uint32_t speed_timestamp = 0, latitude_timestamp = 0, longitude_timestamp = 0;
    int32_t bearing_rpm = 0, brake_pressure = 0;
    int32_t speed_x100 = 0, latitude_e7 = 0, longitude_e7 = 0;
    bool have_bearing = false, have_brake = false;
    bool have_speed = false, have_latitude = false, have_longitude = false;
    while (fread(record, sizeof(record), 1, file) == 1) {
        /* Keep the companion file aligned with every binary record. */
        int64_t utc_ms = read_utc_ms(utc_file);
        uint32_t can_id = (uint32_t)record[0] & 0x7ffU;
        uint32_t timestamp = (uint32_t)(record[1] >> 32);
        int32_t value = (int32_t)(uint32_t)record[1];
        if (can_id == CAN_ID_FRONT_BRAKE) {
            brake_timestamp = timestamp;
            brake_pressure = value;
            have_brake = true;
            continue;
        }
        if (can_id == CAN_ID_BEARING_ENCODER) {
            bearing_timestamp = timestamp;
            bearing_rpm = value;
            have_bearing = true;
            continue;
        }
        if (can_id == CAN_ID_GPS_SPEED) {
            speed_timestamp = timestamp;
            speed_x100 = value;
            have_speed = true;
            continue;
        }
        if (can_id == CAN_ID_GPS_LATITUDE) {
            latitude_timestamp = timestamp;
            latitude_e7 = value;
            have_latitude = true;
            continue;
        }
        if (can_id == CAN_ID_GPS_LONGITUDE) {
            longitude_timestamp = timestamp;
            longitude_e7 = value;
            have_longitude = true;
            continue;
        }
        if (can_id != CAN_ID_ENGINE_RPM || !have_bearing ||
            !sample_at_or_before(timestamp, bearing_timestamp) ||
            (uint32_t)(timestamp - bearing_timestamp) > 100) continue;

        char brake_text[16] = "", speed_text[16] = "";
        char latitude_text[24] = "", longitude_text[24] = "";
        if (have_brake && sample_at_or_before(timestamp, brake_timestamp) &&
            (uint32_t)(timestamp - brake_timestamp) <= 100) {
            snprintf(brake_text, sizeof(brake_text), "%" PRId32, brake_pressure);
        }
        if (have_speed && sample_at_or_before(timestamp, speed_timestamp)) {
            snprintf(speed_text, sizeof(speed_text), "%.2f", speed_x100 / 100.0);
        }
        if (have_latitude && sample_at_or_before(timestamp, latitude_timestamp)) {
            snprintf(latitude_text, sizeof(latitude_text), "%.7f", latitude_e7 / 10000000.0);
        }
        if (have_longitude && sample_at_or_before(timestamp, longitude_timestamp)) {
            snprintf(longitude_text, sizeof(longitude_text), "%.7f",
                     longitude_e7 / 10000000.0);
        }
        char row[256];
        char absolute_time[32];
        format_absolute_time(utc_ms, absolute_time, sizeof(absolute_time));
        int length = snprintf(row, sizeof(row), "%" PRIu32 ",%s,%s,%" PRId32
                              ",%" PRId32 ",%s,%s,%s\r\n", timestamp,
                              absolute_time, brake_text, bearing_rpm, value,
                              latitude_text, longitude_text, speed_text);
        if (length < 0 || length >= (int)sizeof(row) ||
            httpd_resp_send_chunk(request, row, length) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ferror(file) ? ESP_FAIL : httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t stream_csv(httpd_req_t *request, FILE *file, FILE *utc_file)
{
    static const char header[] =
        "sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data\r\n";
    if (httpd_resp_send_chunk(request, header, sizeof(header) - 1) != ESP_OK) {
        return ESP_FAIL;
    }
    uint64_t record[2];
    uint64_t sample_index = 0;
    while (fread(record, sizeof(record), 1, file) == 1) {
        /* Consume one companion entry per binary record. */
        char absolute_time[32];
        format_absolute_time(read_utc_ms(utc_file), absolute_time,
                             sizeof(absolute_time));
        uint32_t can_id = (uint32_t)record[0] & 0x7ffU;
        uint64_t packed = record[1];
        int32_t value = (int32_t)(packed & UINT32_MAX);
        uint32_t timestamp = (uint32_t)(packed >> 32);
        const live_signal_metadata_t *metadata = metadata_for(can_id);
        const char *signal = metadata ? metadata->signal : "";
        const char *node = metadata ? metadata->node : "";
        const char *units = metadata ? metadata->units : "raw";
        char value_text[32];
        if (can_id == CAN_ID_GPS_SPEED) {
            snprintf(value_text, sizeof(value_text), "%.2f", value / 100.0);
            units = "km/h";
        } else {
            snprintf(value_text, sizeof(value_text), "%" PRId32, value);
        }
        char row[256];
        int length = snprintf(row, sizeof(row),
                              "%" PRIu64 ",%" PRIu32 ",0x%03" PRIX32
                              ",%s,%s,%" PRIu32 ",%s,%s,%s,%" PRIu64 "\r\n",
                              sample_index++, can_id, can_id, signal, node,
                              timestamp, absolute_time, value_text, units, packed);
        if (length < 0 || length >= (int)sizeof(row) ||
            httpd_resp_send_chunk(request, row, length) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ferror(file) ? ESP_FAIL : httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t download_handler(httpd_req_t *request)
{
    char query[128], name[DATA_LOGGER_FILENAME_MAX + 1], format[8];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(query, "format", format, sizeof(format)) != ESP_OK ||
        !data_logger_valid_filename(name) ||
        (strcmp(format, "bin") != 0 && strcmp(format, "csv") != 0 &&
         strcmp(format, "paired") != 0)) {
        return send_json_error(request, "400 Bad Request",
                               "Expected a valid log filename and bin, csv or paired format");
    }
    if (data_logger_state() != LOGGER_IDLE &&
        strcmp(name, base_name(data_logger_path())) == 0) {
        return send_json_error(request, "409 Conflict",
                               "The active log is not available until Stop completes");
    }
    if (xSemaphoreTake(download_mutex, 0) != pdTRUE) {
        return send_json_error(request, "503 Service Unavailable",
                               "Another download is already active");
    }
    char path[96];
    snprintf(path, sizeof(path), SD_ROOT "/%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        xSemaphoreGive(download_mutex);
        return send_json_error(request, "404 Not Found", "Log file not found");
    }
    char attachment[80];
    bool paired = strcmp(format, "paired") == 0;
    bool csv_format = paired || strcmp(format, "csv") == 0;
    if (csv_format) {
        char csv_name[48];
        snprintf(csv_name, sizeof(csv_name), "%.*s%s.csv",
                 (int)(strlen(name) - 4), name,
                 paired ? "_powertrain" : "");
        snprintf(attachment, sizeof(attachment), "attachment; filename=\"%s\"", csv_name);
        httpd_resp_set_type(request, "text/csv; charset=utf-8");
    } else {
        snprintf(attachment, sizeof(attachment), "attachment; filename=\"%s\"", name);
        httpd_resp_set_type(request, "application/octet-stream");
    }
    httpd_resp_set_hdr(request, "Content-Disposition", attachment);
    FILE *utc_file = NULL;
    if (csv_format) {
        char utc_path[104];
        snprintf(utc_path, sizeof(utc_path), "%s.utc", path);
        utc_file = fopen(utc_path, "rb");
    }
    esp_err_t result = paired ? stream_paired_csv(request, file, utc_file)
                     : csv_format ? stream_csv(request, file, utc_file)
                     : stream_binary(request, file);
    if (utc_file) fclose(utc_file);
    fclose(file);
    xSemaphoreGive(download_mutex);
    return result;
}

static bool request_token(httpd_req_t *request, uint32_t *token)
{
    char query[64], value[16], *end;
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    unsigned long parsed = strtoul(value, &end, 16);
    if (*value == '\0' || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }
    *token = (uint32_t)parsed;
    return true;
}

static esp_err_t live_signals_handler(httpd_req_t *request)
{
    size_t count;
    const live_signal_metadata_t *signals = live_data_signals(&count);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_send_chunk(request, "[", 1) != ESP_OK) return ESP_FAIL;
    for (size_t i = 0; i < count; i++) {
        char item[192];
        int length = snprintf(item, sizeof(item),
                              "%s{\"can_id\":%" PRIu32 ",\"can_id_hex\":\"0x%03" PRIX32
                              "\",\"signal\":\"%s\",\"node\":\"%s\","
                              "\"units\":\"%s\",\"native_rate_hz\":%u}",
                              i ? "," : "", signals[i].can_id, signals[i].can_id,
                              signals[i].signal, signals[i].node, signals[i].units,
                              signals[i].native_rate_hz);
        if (length < 0 || length >= (int)sizeof(item) ||
            httpd_resp_send_chunk(request, item, length) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(request, "]", 1) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t live_start_handler(httpd_req_t *request)
{
    uint32_t token;
    app_control_result_t result = app_control_start_live_data(&token);
    if (result == APP_CONTROL_CONFLICT) {
        return send_json_error(request, "409 Conflict",
                               "Live data is available only while recording");
    }
    if (result == APP_CONTROL_BUSY) {
        return send_json_error(request, "503 Service Unavailable",
                               "Another live viewer is active");
    }
    if (result != APP_CONTROL_OK) {
        return send_json_error(request, "500 Internal Server Error",
                               "Unable to start live data");
    }
    char body[96];
    snprintf(body, sizeof(body),
             "{\"token\":\"%08" PRIx32 "\",\"lease_seconds\":%u}",
             token, LIVE_DATA_LEASE_SECONDS);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t live_samples_handler(httpd_req_t *request)
{
    uint32_t token;
    if (!request_token(request, &token)) {
        return send_json_error(request, "400 Bad Request", "Missing or invalid token");
    }
    live_sample_t samples[LIVE_DATA_SIGNAL_COUNT];
    size_t count;
    live_data_result_t result =
        live_data_snapshot(token, samples, LIVE_DATA_SIGNAL_COUNT, &count);
    if (result == LIVE_DATA_DISABLED) {
        return send_json_error(request, "409 Conflict", "Live data is disabled");
    }
    if (result != LIVE_DATA_OK) {
        return send_json_error(request, "403 Forbidden", "Invalid live viewer token");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_send_chunk(request, "{\"samples\":[", 12) != ESP_OK) return ESP_FAIL;
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        if (!samples[i].valid) continue;
        char item[160];
        int length = snprintf(item, sizeof(item),
                              "%s{\"can_id\":%" PRIu32 ",\"value\":%" PRId32
                              ",\"timestamp_ms\":%" PRIu32 ",\"sequence\":%" PRIu32 "}",
                              first ? "" : ",", samples[i].can_id, samples[i].value,
                              samples[i].timestamp_ms, samples[i].sequence);
        if (length < 0 || length >= (int)sizeof(item) ||
            httpd_resp_send_chunk(request, item, length) != ESP_OK) return ESP_FAIL;
        first = false;
    }
    if (httpd_resp_send_chunk(request, "]}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t live_stop_handler(httpd_req_t *request)
{
    uint32_t token;
    if (!request_token(request, &token)) {
        return send_json_error(request, "400 Bad Request", "Missing or invalid token");
    }
    live_data_result_t result = live_data_stop(token);
    if (result == LIVE_DATA_INVALID_TOKEN) {
        return send_json_error(request, "403 Forbidden", "Invalid live viewer token");
    }
    if (result == LIVE_DATA_DISABLED) {
        return send_json_error(request, "409 Conflict", "Live data is disabled");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"live_enabled\":false}");
}
static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) return error;
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/logging/start", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/logging/stop", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
        {.uri = "/api/logs/rename", .method = HTTP_POST, .handler = rename_log_handler},
        {.uri = "/api/logs/download", .method = HTTP_GET, .handler = download_handler},
        {.uri = "/api/live/signals", .method = HTTP_GET, .handler = live_signals_handler},
        {.uri = "/api/live/start", .method = HTTP_POST, .handler = live_start_handler},
        {.uri = "/api/live/samples", .method = HTTP_GET, .handler = live_samples_handler},
        {.uri = "/api/live/stop", .method = HTTP_POST, .handler = live_stop_handler},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        error = httpd_register_uri_handler(server, &handlers[i]);
        if (error != ESP_OK) {
            httpd_stop(server);
            server = NULL;
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    download_mutex = xSemaphoreCreateMutex();
    if (!download_mutex) return ESP_ERR_NO_MEM;
    esp_err_t error = esp_hosted_init();
    if (error != ESP_OK) return error;
    error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) return error;
    if ((error = esp_netif_init()) != ESP_OK) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_ERR_NO_MEM;
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_err_t dhcp_error = esp_netif_dhcps_stop(ap);
    if (dhcp_error != ESP_OK &&
        dhcp_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return dhcp_error;
    }
    if ((error = esp_netif_set_ip_info(ap, &ip)) != ESP_OK ||
        (error = esp_netif_dhcps_start(ap)) != ESP_OK) {
        return error;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&init)) != ESP_OK) return error;
    wifi_config_t config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = sizeof(AP_SSID) - 1,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    if ((error = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (error = esp_wifi_set_config(WIFI_IF_AP, &config)) != ESP_OK ||
        (error = esp_wifi_start()) != ESP_OK) {
        return error;
    }
    error = start_http_server();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Open AP '%s' ready at http://192.168.4.1", AP_SSID);
    }
    return error;
}
