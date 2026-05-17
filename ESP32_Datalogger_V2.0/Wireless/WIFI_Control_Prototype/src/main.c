#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_control";

#define WIFI_AP_SSID "BajaLogger"
#define WIFI_AP_PASS "bajalogger"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_STA_CONN 4

static SemaphoreHandle_t s_hosted_transport_up;

static const char s_index_html[] =
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Baja Logger</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;margin:24px;max-width:760px}"
    "button{font-size:18px;margin:6px 6px 6px 0;padding:10px 14px}"
    "pre{background:#111;color:#eee;padding:14px;overflow:auto}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>Baja Logger Wi-Fi Control</h1>"
    "<p>This is the standalone Wi-Fi proof-of-life project.</p>"
    "<button onclick=\"callApi('/api/start')\">Start</button>"
    "<button onclick=\"callApi('/api/stop')\">Stop</button>"
    "<button onclick=\"callApi('/api/status')\">Status</button>"
    "<button onclick=\"callApi('/api/files')\">Files</button>"
    "<pre id=\"out\">Ready.</pre>"
    "<script>"
    "async function callApi(path){"
    "const r=await fetch(path);"
    "document.getElementById('out').textContent=await r.text();"
    "}"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t send_text(httpd_req_t *req, const char *type, const char *text)
{
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_text(req, "text/html", s_index_html);
}

static esp_err_t api_start_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP start requested");
    return send_text(req, "application/json",
                     "{\"ok\":true,\"command\":\"start\",\"note\":\"prototype only\"}");
}

static esp_err_t api_stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP stop requested");
    return send_text(req, "application/json",
                     "{\"ok\":true,\"command\":\"stop\",\"note\":\"prototype only\"}");
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    return send_text(req, "application/json",
                     "{\"ok\":true,\"project\":\"wifi_control_prototype\","
                     "\"wifi_mode\":\"ap\",\"ssid\":\"BajaLogger\"}");
}

static esp_err_t api_files_handler(httpd_req_t *req)
{
    return send_text(req, "application/json",
                     "{\"ok\":true,\"files\":[],\"note\":\"prototype only\"}");
}

static httpd_handle_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/start", .method = HTTP_GET, .handler = api_start_handler},
        {.uri = "/api/stop", .method = HTTP_GET, .handler = api_stop_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler},
        {.uri = "/api/files", .method = HTTP_GET, .handler = api_files_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected: aid=%d", event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected: aid=%d", event->aid);
    }
}

static void hosted_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        ESP_LOGI(TAG, "ESP-Hosted transport is up");
        xSemaphoreGive(s_hosted_transport_up);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        ESP_LOGW(TAG, "ESP-Hosted transport is down");
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        ESP_LOGE(TAG, "ESP-Hosted transport failure");
    } else if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        ESP_LOGI(TAG, "ESP-Hosted coprocessor init event");
    }
}

static esp_err_t hosted_init_and_wait(void)
{
    s_hosted_transport_up = xSemaphoreCreateBinary();
    if (s_hosted_transport_up == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HOSTED_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               hosted_event_handler,
                                               NULL));

    ESP_RETURN_ON_FALSE(esp_hosted_init() == ESP_OK, ESP_FAIL, TAG, "esp_hosted_init failed");
    ESP_RETURN_ON_FALSE(esp_hosted_connect_to_slave() == ESP_OK,
                        ESP_FAIL,
                        TAG,
                        "esp_hosted_connect_to_slave failed");

    ESP_LOGI(TAG, "Waiting for ESP-Hosted transport...");
    if (xSemaphoreTake(s_hosted_transport_up, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for ESP-Hosted transport");
        return ESP_ERR_TIMEOUT;
    }

    esp_hosted_coprocessor_fwver_t fwver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Coprocessor FW version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fwver.major1,
                 fwver.minor1,
                 fwver.patch1);
    } else {
        ESP_LOGW(TAG, "Could not read coprocessor FW version");
    }

    return ESP_OK;
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(hosted_init_and_wait());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               wifi_event_handler,
                                               NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, WIFI_AP_PASS, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG,
             "Wi-Fi AP started. SSID=%s password=%s channel=%d URL=http://192.168.4.1/",
             WIFI_AP_SSID,
             WIFI_AP_PASS,
             WIFI_AP_CHANNEL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_softap();
    (void)start_http_server();
}
