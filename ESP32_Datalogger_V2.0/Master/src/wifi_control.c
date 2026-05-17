#include "wifi_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_control";

#define WIFI_STA_CONFIG_PATH "/sdcard/wifi_sta_config.json"
#define WIFI_STA_CONFIG_MAX_BYTES 1024
#define WIFI_STA_MAX_RETRY 10

#define WIFI_STA_FALLBACK_SSID "BajaLogger"
#define WIFI_STA_FALLBACK_PASSWORD "bajalogger"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

typedef struct {
    char ssid[33];
    char password[65];
} wifi_sta_config_file_t;

static SemaphoreHandle_t s_hosted_transport_up;
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;

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
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_STA_MAX_RETRY) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected; retrying (%d/%d)",
                     s_wifi_retry_count,
                     WIFI_STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected. IP=" IPSTR, IP2STR(&event->ip_info.ip));
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

    esp_netif_create_default_wifi_sta();

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
