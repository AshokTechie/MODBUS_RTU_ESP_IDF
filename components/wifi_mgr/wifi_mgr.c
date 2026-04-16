#include "wifi_mgr.h"
#include "storage.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CFG_NAMESPACE_PRIMARY "smartload_cfg"
static const char WIFI_CFG_NAMESPACE_LEGACY[] = { 0x72, 0x64, 0x75, 0x5f, 0x63, 0x66, 0x67, 0x00 };

#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1
#define WIFI_MAX_RETRY            10
#define WIFI_CONNECT_TIMEOUT_MS  30000

static EventGroupHandle_t s_evt_grp        = NULL;
static esp_netif_t       *s_netif_sta      = NULL;
static bool               s_connected      = false;
static bool               s_ever_connected = false;
static int                s_retry_count    = 0;

static esp_err_t load_credentials_from_nvs_namespace(const char *ns,
                                                     char *ssid, size_t ssid_size,
                                                     char *pass, size_t pass_size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_size;
    if (nvs_get_str(nvs, "wifi_ssid", ssid, &len) != ESP_OK) ssid[0] = '\0';
    len = pass_size;
    if (nvs_get_str(nvs, "wifi_pass", pass, &len) != ESP_OK) pass[0] = '\0';
    nvs_close(nvs);
    return ssid[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void trim_whitespace(char *str)
{
    char *start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    size_t len = strlen(str);
    while (len > 0) {
        char c = str[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        str[--len] = '\0';
    }
}

static bool parse_wifi_json(const char *json, size_t json_len,
                            char *ssid, size_t ssid_size,
                            char *pass, size_t pass_size)
{
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) return false;

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    cJSON *work = (wifi && cJSON_IsObject(wifi)) ? wifi : root;

    cJSON *s = cJSON_GetObjectItem(work, "ssid");
    cJSON *p = cJSON_GetObjectItem(work, "password");
    if (!p) p = cJSON_GetObjectItem(work, "pass");

    bool ok = false;
    if (s && cJSON_IsString(s) && s->valuestring[0] != '\0') {
        snprintf(ssid, ssid_size, "%s", s->valuestring);
        if (p && cJSON_IsString(p)) {
            snprintf(pass, pass_size, "%s", p->valuestring);
        }
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

static bool parse_wifi_text(const char *text,
                            char *ssid, size_t ssid_size,
                            char *pass, size_t pass_size)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "%s", text);

    char *lines[4] = {0};
    int line_count = 0;
    char *next = strtok(buf, "\r\n");
    while (next && line_count < 4) {
        trim_whitespace(next);
        if (next[0] != '\0') {
            lines[line_count++] = next;
        }
        next = strtok(NULL, "\r\n");
    }

    for (int i = 0; i < line_count; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq) eq = strchr(lines[i], ':');
        if (!eq) continue;

        *eq = '\0';
        char *key = lines[i];
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);

        if ((strcasecmp(key, "ssid") == 0 || strcasecmp(key, "wifi_ssid") == 0) && value[0] != '\0') {
            snprintf(ssid, ssid_size, "%s", value);
        } else if (strcasecmp(key, "password") == 0 ||
                   strcasecmp(key, "pass") == 0 ||
                   strcasecmp(key, "wifi_pass") == 0) {
            snprintf(pass, pass_size, "%s", value);
        }
    }

    if (ssid[0] == '\0' && line_count >= 1) {
        snprintf(ssid, ssid_size, "%s", lines[0]);
        if (line_count >= 2) {
            snprintf(pass, pass_size, "%s", lines[1]);
        }
    }

    trim_whitespace(ssid);
    trim_whitespace(pass);
    return ssid[0] != '\0';
}

static bool parse_wifi_blob(const char *blob, size_t blob_len,
                            char *ssid, size_t ssid_size,
                            char *pass, size_t pass_size)
{
    if (parse_wifi_json(blob, blob_len, ssid, ssid_size, pass, pass_size)) {
        return true;
    }

    return parse_wifi_text(blob, ssid, ssid_size, pass, pass_size);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();

        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "Disconnected (reason=%d)", evt->reason);

            if (!s_ever_connected) {
                if (s_retry_count < WIFI_MAX_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_evt_grp, WIFI_FAIL_BIT);
                }
            } else {
                ESP_LOGI(TAG, "Reconnecting...");
                esp_wifi_connect();
            }
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        s_ever_connected = true;
        xEventGroupSetBits(s_evt_grp, WIFI_CONNECTED_BIT);
    }
}

static void load_credentials(char *ssid, size_t ssid_size,
                             char *pass, size_t pass_size)
{
    ssid[0] = '\0';
    pass[0] = '\0';

    if (load_credentials_from_nvs_namespace(WIFI_CFG_NAMESPACE_PRIMARY, ssid, ssid_size, pass, pass_size) == ESP_OK) {
        ESP_LOGI(TAG, "Credentials from NVS/%s (ssid=%s)", WIFI_CFG_NAMESPACE_PRIMARY, ssid);
        return;
    }

    if (load_credentials_from_nvs_namespace(WIFI_CFG_NAMESPACE_LEGACY, ssid, ssid_size, pass, pass_size) == ESP_OK) {
        ESP_LOGI(TAG, "Credentials from NVS/%s (ssid=%s)", WIFI_CFG_NAMESPACE_LEGACY, ssid);
        return;
    }

    static char file_buf[256];
    size_t file_len = 0;

    if (storage_read("/wifi.json", file_buf, sizeof(file_buf), &file_len) == ESP_OK &&
        parse_wifi_json(file_buf, file_len, ssid, ssid_size, pass, pass_size)) {
        ESP_LOGI(TAG, "Credentials from /wifi.json (ssid=%s)", ssid);
        return;
    }

    if (storage_read("/wifi.txt", file_buf, sizeof(file_buf), &file_len) == ESP_OK &&
        parse_wifi_blob(file_buf, file_len, ssid, ssid_size, pass, pass_size)) {
        ESP_LOGI(TAG, "Credentials from /wifi.txt (ssid=%s)", ssid);
        return;
    }

    if (storage_read("/config.json", file_buf, sizeof(file_buf), &file_len) == ESP_OK &&
        parse_wifi_json(file_buf, file_len, ssid, ssid_size, pass, pass_size)) {
        ESP_LOGI(TAG, "Credentials from /config.json (ssid=%s)", ssid);
        return;
    }

    if (DEFAULT_WIFI_SSID[0] != '\0') {
        snprintf(ssid, ssid_size, "%s", DEFAULT_WIFI_SSID);
        snprintf(pass, pass_size, "%s", DEFAULT_WIFI_PASS);
        ESP_LOGI(TAG, "Credentials from build defaults (ssid=%s)", ssid);
    }
}

esp_err_t wifi_mgr_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();

    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
    }

    if (!s_evt_grp) {
        s_evt_grp = xEventGroupCreate();
        if (!s_evt_grp) {
            ESP_LOGE(TAG, "Event group creation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    ESP_LOGI(TAG, "WiFi manager initialised");
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(void)
{
    if (!s_evt_grp) return ESP_ERR_INVALID_STATE;

    xEventGroupClearBits(s_evt_grp, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    static char ssid[32];
    static char pass[64];
    load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "No WiFi credentials - provision via NVS, /wifi.json, /wifi.txt, /config.json, or build flag");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);
    cfg.sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_evt_grp,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAIL_BIT) return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_mgr_disconnect(void)
{
    esp_wifi_stop();
    s_connected = false;
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_CFG_NAMESPACE_PRIMARY, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", password);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    static char json_buf[192];
    snprintf(json_buf, sizeof(json_buf),
             "{\"ssid\":\"%s\",\"password\":\"%s\"}", ssid, password);
    esp_err_t err = storage_write("/wifi.json", json_buf);
    ESP_LOGI(TAG, "Credentials saved (ssid=%s)", ssid);
    return err;
}
