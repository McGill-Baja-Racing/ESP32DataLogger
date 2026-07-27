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
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#define AP_SSID "BajaDAQ"
#define SD_ROOT "/sdcard"
#define DOWNLOAD_CHUNK_SIZE 4096

typedef struct {
    char name[32];
    off_t size;
    unsigned index;
} log_entry_t;

typedef struct {
    uint32_t can_id;
    const char *signal;
    const char *node;
    const char *units;
} signal_metadata_t;

static const char *TAG = "WebServer";
static httpd_handle_t server;
static SemaphoreHandle_t download_mutex;

static const signal_metadata_t signal_metadata[] = {
    {0x0B1, "front_brake_pressure", "brake_node_1", "psi"},
    {0x0B2, "rear_brake_pressure", "brake_node_1", "psi"},
    {0x0B9, "bearing_rpm", "encoder_node_4", "rpm"},
    {0x0BA, "generic_adc_voltage", "adc_node_6", "mV"},
    {0x0BB, "engine_rpm", "engine_node_5", "rpm_placeholder"},
};

static const char index_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Baja DAQ</title><style>"
    ":root{font-family:system-ui,sans-serif;color:#17202a;background:#f2f4f7}"
    "body{max-width:760px;margin:auto;padding:18px}h1{margin:0 0 16px}"
    ".card{background:white;border-radius:12px;padding:16px;margin:12px 0;"
    "box-shadow:0 2px 9px #0001}.state{font-size:1.3rem;font-weight:700}"
    "button,.download{border:0;border-radius:8px;padding:12px 18px;margin:5px;"
    "font-weight:650;text-decoration:none;display:inline-block;cursor:pointer}"
    "#start{background:#16833b;color:white}#stop{background:#c53232;color:white}"
    "button:disabled{opacity:.4;cursor:not-allowed}.download{background:#e8edf3;color:#17202a}"
    ".row{display:flex;justify-content:space-between;gap:10px;align-items:center;"
    "border-top:1px solid #e7e9ec;padding:10px 0;flex-wrap:wrap}"
    ".muted{color:#65707c;font-size:.9rem}.error{color:#b42318}</style></head>"
    "<body><h1>Baja DAQ</h1><section class=card>"
    "<div id=state class=state>Connecting...</div><div id=file class=muted></div>"
    "<p><button id=start>Start</button>"
    "<button id=stop>Stop</button></p>"
    "<div id=stats class=muted></div><div id=nodes class=muted></div>"
    "<div id=error class=error></div></section>"
    "<section class=card><h2>Completed logs</h2><div id=logs>Loading...</div></section>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "async function json(url,opt){const r=await fetch(url,opt);"
    "const body=await r.json().catch(()=>({error:'Request failed'}));"
    "if(!r.ok)throw Error(body.error||('HTTP '+r.status));return body}"
    "async function refresh(){try{const s=await json('/api/status');"
    "$('state').textContent=s.logger_state[0].toUpperCase()+s.logger_state.slice(1);"
    "$('file').textContent=s.current_file==='none'?'No session yet':s.current_file;"
    "$('stats').textContent=`CAN drops: ${s.can_drops} | Log drops: ${s.log_drops}`;"
    "$('nodes').textContent=`Nodes — 1: ${s.nodes['1']}, 4: ${s.nodes['4']}, "
    "5: ${s.nodes['5']}, 6: ${s.nodes['6']}`;"
    "$('start').disabled=s.logger_state!=='idle';"
    "$('stop').disabled=s.logger_state!=='running';$('error').textContent='';"
    "await refreshLogs()}catch(e){$('error').textContent=e.message}}"
    "async function sendLoggingCommand(action){try{await json('/api/logging/'+action,{method:'POST'});"
    "await refresh()}catch(e){$('error').textContent=e.message}}"
    "async function refreshLogs(){const logs=await json('/api/logs');"
    "$('logs').innerHTML=logs.length?'':'No completed sessions';"
    "for(const l of logs){const row=document.createElement('div');row.className='row';"
    "const label=document.createElement('span');"
    "label.textContent=`${l.name} (${(l.size_bytes/1024).toFixed(1)} KiB)`;row.append(label);"
    "const actions=document.createElement('span');"
    "for(const f of ['bin','csv']){const a=document.createElement('a');a.className='download';"
    "a.textContent=f.toUpperCase();a.href='/api/logs/download?name='+"
    "encodeURIComponent(l.name)+'&format='+f;actions.append(a)}row.append(actions);$('logs').append(row)}}"
    "$('start').addEventListener('click',()=>sendLoggingCommand('start'));"
    "$('stop').addEventListener('click',()=>sendLoggingCommand('stop'));"
    "refresh();setInterval(refresh,1000);</script></body></html>";

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
    return httpd_resp_send(request, index_html, sizeof(index_html) - 1);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    app_status_t status;
    char body[512];
    app_control_get_status(&status);
    snprintf(body, sizeof(body),
             "{\"logger_state\":\"%s\",\"current_file\":\"%s\","
             "\"can_drops\":%" PRIu32 ",\"log_drops\":%" PRIu32 ","
             "\"nodes\":{\"1\":\"%s\",\"4\":\"%s\",\"5\":\"%s\","
             "\"6\":\"%s\"}}",
             data_logger_state_name(), base_name(status.current_file),
             status.can_drops, status.log_drops, status.node_1, status.node_4,
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

static const signal_metadata_t *metadata_for(uint32_t can_id)
{
    for (size_t i = 0; i < sizeof(signal_metadata) / sizeof(signal_metadata[0]); i++) {
        if (signal_metadata[i].can_id == can_id) return &signal_metadata[i];
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
        const signal_metadata_t *metadata = metadata_for(can_id);
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

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) return error;
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/logging/start", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/logging/stop", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
        {.uri = "/api/logs/download", .method = HTTP_GET, .handler = download_handler},
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
