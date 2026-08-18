#include "web/web_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "protocol/app_protocol.h"

#define AP_SSID "BajaDAQ"
#define SD_ROOT "/sdcard"
#define DOWNLOAD_CHUNK_SIZE 4096

typedef struct {
    char name[32];
    off_t size;
    unsigned index;
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

static bool parse_log_name(const char *name, unsigned *index)
{
    if (!name || strncmp(name, "log_", 4) != 0) return false;
    const char *digits = name + 4;
    const char *suffix = strstr(digits, ".bin");
    if (!suffix || suffix[4] != '\0' || suffix - digits < 4) return false;
    unsigned value = 0;
    for (const char *p = digits; p < suffix; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        value = value * 10U + (unsigned)(*p - '0');
    }
    if (index) *index = value;
    return true;
}

static int newest_first(const void *left, const void *right)
{
    const log_entry_t *a = left;
    const log_entry_t *b = right;
    return a->index < b->index ? 1 : a->index > b->index ? -1 : 0;
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
        unsigned index;
        if (!parse_log_name(item->d_name, &index) ||
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
        entries[*count].index = index;
        (*count)++;
    }
    closedir(directory);
    qsort(entries, *count, sizeof(*entries), newest_first);
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
             "\"live_enabled\":%s,"
             "\"nodes\":{\"1\":\"%s\",\"4\":\"%s\",\"5\":\"%s\","
             "\"6\":\"%s\"}}",
             data_logger_state_name(), base_name(status.current_file),
             status.can_drops, status.log_drops,
             status.live_enabled ? "true" : "false", status.node_1, status.node_4,
             status.node_5, status.node_6);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t command_handler(httpd_req_t *request)
{
    app_control_result_t result =
        strcmp(request->uri, "/api/logging/start") == 0
        ? app_control_start_logging() : app_control_stop_logging();
    if (result == APP_CONTROL_CONFLICT) {
        return send_json_error(request, "409 Conflict",
                               "Command is not valid in the current logger state");
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

static const live_signal_metadata_t *metadata_for(uint32_t can_id)
{
    static const live_signal_metadata_t engine_spark = {
        CAN_ID_ENGINE_SPARK, "engine_spark", "engine_node_5", "event", 67
    };
    size_t count;
    const live_signal_metadata_t *signals = live_data_signals(&count);
    for (size_t i = 0; i < count; i++) {
        if (signals[i].can_id == can_id) return &signals[i];
    }
    if (can_id == CAN_ID_ENGINE_SPARK) return &engine_spark;
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

static esp_err_t stream_csv(httpd_req_t *request, FILE *file)
{
    static const char header[] =
        "sample_index,can_id,can_id_hex,signal,node,timestamp_ms,value,units,raw_data\r\n";
    if (httpd_resp_send_chunk(request, header, sizeof(header) - 1) != ESP_OK) {
        return ESP_FAIL;
    }
    uint64_t record[2];
    uint64_t sample_index = 0;
    while (fread(record, sizeof(record), 1, file) == 1) {
        uint32_t can_id = (uint32_t)record[0] & 0x7ffU;
        uint64_t packed = record[1];
        int32_t value = (int32_t)(packed & UINT32_MAX);
        uint32_t timestamp = (uint32_t)(packed >> 32);
        const live_signal_metadata_t *metadata = metadata_for(can_id);
        const char *signal = metadata ? metadata->signal : "";
        const char *node = metadata ? metadata->node : "";
        const char *units = metadata ? metadata->units : "raw";
        char row[256];
        int length = snprintf(row, sizeof(row),
                              "%" PRIu64 ",%" PRIu32 ",0x%03" PRIX32
                              ",%s,%s,%" PRIu32 ",%" PRId32 ",%s,%" PRIu64 "\r\n",
                              sample_index++, can_id, can_id, signal, node,
                              timestamp, value, units, packed);
        if (length < 0 || length >= (int)sizeof(row) ||
            httpd_resp_send_chunk(request, row, length) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ferror(file) ? ESP_FAIL : httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t download_handler(httpd_req_t *request)
{
    char query[128], name[32], format[8];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(query, "format", format, sizeof(format)) != ESP_OK ||
        !parse_log_name(name, NULL) ||
        (strcmp(format, "bin") != 0 && strcmp(format, "csv") != 0)) {
        return send_json_error(request, "400 Bad Request",
                               "Expected a valid log filename and bin or csv format");
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
    if (strcmp(format, "csv") == 0) {
        char csv_name[32];
        snprintf(csv_name, sizeof(csv_name), "%.*s.csv",
                 (int)(strlen(name) - 4), name);
        snprintf(attachment, sizeof(attachment), "attachment; filename=\"%s\"", csv_name);
        httpd_resp_set_type(request, "text/csv; charset=utf-8");
    } else {
        snprintf(attachment, sizeof(attachment), "attachment; filename=\"%s\"", name);
        httpd_resp_set_type(request, "application/octet-stream");
    }
    httpd_resp_set_hdr(request, "Content-Disposition", attachment);
    esp_err_t result = strcmp(format, "csv") == 0
                     ? stream_csv(request, file) : stream_binary(request, file);
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
