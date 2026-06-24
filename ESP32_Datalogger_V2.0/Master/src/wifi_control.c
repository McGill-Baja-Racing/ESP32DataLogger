#include "wifi_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_control";

#define WIFI_STA_CONFIG_PATH "/sdcard/wifi_sta_config.json"
#define WIFI_STA_CONFIG_MAX_BYTES 1024
#define WIFI_STA_MAX_RETRY 10
#define WIFI_RECOVERY_MIN_INTERVAL_MS 15000
#define WIFI_RECOVERY_RESET_DELAY_MS 500
#define WIFI_RECOVERY_CONNECT_WAIT_MS 15000

#define WIFI_STA_FALLBACK_SSID "Frank-Lucas"
#define WIFI_STA_FALLBACK_PASSWORD "frank1234"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

typedef struct {
    char ssid[33];
    char password[65];
} wifi_sta_config_file_t;

static SemaphoreHandle_t s_hosted_transport_up;
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static int s_wifi_retry_count;
static bool s_wifi_connected_once;
static volatile bool s_wifi_reconnect_pending;
static volatile bool s_wifi_recovery_active;
static int64_t s_last_wifi_reconnect_us;

static char *read_text_file(const char *path, size_t max_bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);
    if (file_size < 0 || (size_t)file_size > max_bytes) {
        fclose(file);
        ESP_LOGE(TAG, "Wi-Fi config too large: %s", path);
        return NULL;
    }

    rewind(file);

    char *text = calloc(1, (size_t)file_size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }

    size_t read_len = fread(text, 1, (size_t)file_size, file);
    fclose(file);

    if (read_len != (size_t)file_size) {
        free(text);
        return NULL;
    }

    return text;
}

static esp_err_t load_wifi_sta_config(wifi_sta_config_file_t *config)
{
    memset(config, 0, sizeof(*config));

    char *text = read_text_file(WIFI_STA_CONFIG_PATH, WIFI_STA_CONFIG_MAX_BYTES);
    if (!text) {
        snprintf(config->ssid, sizeof(config->ssid), "%s", WIFI_STA_FALLBACK_SSID);
        snprintf(config->password, sizeof(config->password), "%s", WIFI_STA_FALLBACK_PASSWORD);
        ESP_LOGW(TAG, "Missing %s; using hardcoded fallback SSID=%s",
                 WIFI_STA_CONFIG_PATH,
                 config->ssid);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", WIFI_STA_CONFIG_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Wi-Fi config must include non-empty string field: ssid");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(config->ssid, sizeof(config->ssid), "%s", ssid->valuestring);
    if (cJSON_IsString(password) && password->valuestring) {
        snprintf(config->password, sizeof(config->password), "%s", password->valuestring);
    }

    cJSON_Delete(root);
    return ESP_OK;
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

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ESP_HOSTED_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   hosted_event_handler,
                                                   NULL),
                        TAG,
                        "ESP-Hosted event handler register failed");

    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init failed");
    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "esp_hosted_connect_to_slave failed");

    ESP_LOGI(TAG, "Waiting for ESP-Hosted transport...");
    if (xSemaphoreTake(s_hosted_transport_up, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for ESP-Hosted transport");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_recovery_active) {
            ESP_LOGI(TAG, "Wi-Fi STA started during recovery; connect will be issued by recovery task");
            return;
        }
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_wifi_recovery_active) {
            ESP_LOGW(TAG, "Wi-Fi disconnected during controlled recovery");
            return;
        }

        s_wifi_retry_count++;
        if (!s_wifi_connected_once && s_wifi_retry_count > WIFI_STA_MAX_RETRY) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            return;
        }

        ESP_LOGW(TAG,
                 "Wi-Fi disconnected; retrying (%d%s)",
                 s_wifi_retry_count,
                 s_wifi_connected_once ? "" : "/10");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        s_wifi_connected_once = true;
        ESP_LOGI(TAG, "Wi-Fi connected. IP=" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_sta_netif) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
            esp_err_t dns_err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
            if (dns_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set main DNS: %s", esp_err_to_name(dns_err));
            }

            dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
            dns_err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
            if (dns_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set backup DNS: %s", esp_err_to_name(dns_err));
            } else {
                ESP_LOGI(TAG, "DNS servers set to 8.8.8.8 and 1.1.1.1");
            }
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    wifi_sta_config_file_t sta_config = {0};
    ESP_RETURN_ON_ERROR(load_wifi_sta_config(&sta_config), TAG, "load_wifi_sta_config failed");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");
    ESP_RETURN_ON_ERROR(hosted_init_and_wait(), TAG, "ESP-Hosted init failed");

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   wifi_event_handler,
                                                   NULL),
                        TAG,
                        "Wi-Fi event handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler,
                                                   NULL),
                        TAG,
                        "IP event handler register failed");

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", sta_config.ssid);
    snprintf((char *)wifi_config.sta.password,
             sizeof(wifi_config.sta.password),
             "%s",
             sta_config.password);
    wifi_config.sta.threshold.authmode = strlen(sta_config.password) > 0
                                       ? WIFI_AUTH_WPA2_PSK
                                       : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG,
                        "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID=%s", sta_config.ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi STA ready for internet telemetry");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to Wi-Fi SSID=%s", sta_config.ssid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_control_start(void)
{
    return wifi_init_sta();
}

bool wifi_control_is_connected(void)
{
    return s_wifi_event_group &&
           (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT);
}

static void wifi_reconnect_task(void *arg)
{
    char reason[96] = {};
    if (arg) {
        snprintf(reason, sizeof(reason), "%s", (const char *)arg);
        free(arg);
    }

    ESP_LOGW(TAG,
             "Cycling Wi-Fi STA after connectivity failure%s%s",
             reason[0] ? ": " : "",
             reason);

    s_wifi_recovery_active = true;
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect during recovery failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(WIFI_RECOVERY_RESET_DELAY_MS));

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop during recovery failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(WIFI_RECOVERY_RESET_DELAY_MS));

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_start during recovery failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(WIFI_RECOVERY_RESET_DELAY_MS));

    s_wifi_retry_count = 0;
    s_wifi_recovery_active = false;
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect after recovery reset failed: %s", esp_err_to_name(err));
    }

    if (s_wifi_event_group) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(WIFI_RECOVERY_CONNECT_WAIT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi recovery completed with fresh IP");
        } else {
            ESP_LOGW(TAG, "Wi-Fi recovery did not regain IP within %d ms",
                     WIFI_RECOVERY_CONNECT_WAIT_MS);
        }
    }

    s_wifi_reconnect_pending = false;
    vTaskDelete(NULL);
}

esp_err_t wifi_control_reconnect(const char *reason)
{
    if (!s_wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_wifi_reconnect_pending ||
        (s_last_wifi_reconnect_us > 0 &&
         now_us - s_last_wifi_reconnect_us <
             (int64_t)WIFI_RECOVERY_MIN_INTERVAL_MS * 1000)) {
        return ESP_OK;
    }

    char *reason_copy = NULL;
    if (reason && reason[0]) {
        reason_copy = strndup(reason, 95);
        if (!reason_copy) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_wifi_reconnect_pending = true;
    s_last_wifi_reconnect_us = now_us;

    BaseType_t ok = xTaskCreate(wifi_reconnect_task,
                                "wifi_recover",
                                3072,
                                reason_copy,
                                4,
                                NULL);
    if (ok != pdPASS) {
        free(reason_copy);
        s_wifi_reconnect_pending = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
