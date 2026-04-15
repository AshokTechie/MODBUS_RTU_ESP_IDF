#include "azure_iot.h"

#include "app_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_mgr.h"
#include "ota.h"
#include "sas_token.h"
#include "storage.h"
#include "wifi_mgr.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "azure_iot";

#define SAS_DURATION_SECS (30ULL * 24ULL * 60ULL * 60ULL)

static struct {
    app_config_azure_t creds;
    app_runtime_config_t runtime_cfg;
    char sas_token[256];
    char mqtt_uri[192];
    char mqtt_username[320];
    char telemetry_topic[192];
    bool connected;
} s;

static TaskHandle_t s_bg_task = NULL;
static azure_iot_twin_cb_t s_twin_cb = NULL;
static azure_iot_method_cb_t s_method_cb = NULL;

static esp_err_t build_sd_file_list_json(char *out, size_t out_size)
{
    if (!storage_sd_is_available()) {
        snprintf(out, out_size, "{\"status\":\"sd_unavailable\"}");
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(STORAGE_SD_MOUNT);
    if (!dir) {
        snprintf(out, out_size, "{\"status\":\"open_failed\"}");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    struct dirent *entry = NULL;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        cJSON_AddItemToArray(files, cJSON_CreateString(entry->d_name));
        count++;
    }
    closedir(dir);

    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "count", count);

    bool ok = cJSON_PrintPreallocated(root, out, out_size, false);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t clear_sd_root(bool preserve_config, int *removed_count)
{
    if (removed_count) {
        *removed_count = 0;
    }
    if (!storage_sd_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(STORAGE_SD_MOUNT);
    if (!dir) {
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (preserve_config) {
            if (strcasecmp(entry->d_name, "wifi.txt") == 0 ||
                strcasecmp(entry->d_name, "wifi.json") == 0 ||
                strcasecmp(entry->d_name, "config.json") == 0 ||
                strcasecmp(entry->d_name, "mqtt_config.json") == 0 ||
                strcasecmp(entry->d_name, "azure_config.json") == 0 ||
                strcasecmp(entry->d_name, "update.bin") == 0) {
                continue;
            }
        }

        char full_path[192];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_SD_MOUNT, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(full_path)) {
            continue;
        }

        if (remove(full_path) == 0 && removed_count) {
            (*removed_count)++;
        }
    }

    closedir(dir);
    return ESP_OK;
}

static void send_method_response(const char *rid, int status, const char *payload)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "$iothub/methods/res/%d/?$rid=%s", status, rid);
    mqtt_mgr_publish(topic, payload, 1, 0);
}

static void extract_rid(const char *topic, int topic_len, char *rid, size_t rid_size)
{
    const char *p = strstr(topic, "?$rid=");
    if (!p) {
        snprintf(rid, rid_size, "0");
        return;
    }
    p += 6;
    size_t len = 0;
    while (p[len] && p[len] != '&' && (p + len) < (topic + topic_len)) {
        len++;
    }
    if (len >= rid_size) {
        len = rid_size - 1;
    }
    memcpy(rid, p, len);
    rid[len] = '\0';
}

static void extract_method_name(const char *topic, int topic_len, char *name, size_t name_size)
{
    const char *prefix = "$iothub/methods/POST/";
    const char *start = topic + strlen(prefix);
    const char *end = strchr(start, '/');
    if (!end || end > topic + topic_len) {
        end = topic + topic_len;
    }
    size_t len = (size_t)(end - start);
    if (len >= name_size) {
        len = name_size - 1;
    }
    memcpy(name, start, len);
    name[len] = '\0';
}

static void handle_twin_message(const char *data, int data_len)
{
    char buf[1024];
    int len = data_len < (int)(sizeof(buf) - 1) ? data_len : (int)(sizeof(buf) - 1);
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        return;
    }

    cJSON *desired = cJSON_GetObjectItem(root, "desired");
    cJSON *work = desired ? desired : root;

    cJSON *ro = cJSON_GetObjectItem(work, "ro_code");
    cJSON *dn = cJSON_GetObjectItem(work, "device_name");
    cJSON *ti = cJSON_GetObjectItem(work, "telemetry_interval_sec");

    if (cJSON_IsString(ro)) snprintf(s.runtime_cfg.ro_code, sizeof(s.runtime_cfg.ro_code), "%s", ro->valuestring);
    if (cJSON_IsString(dn)) snprintf(s.runtime_cfg.device_name, sizeof(s.runtime_cfg.device_name), "%s", dn->valuestring);
    if (cJSON_IsNumber(ti)) s.runtime_cfg.telemetry_interval_sec = (int)ti->valuedouble;

    app_config_save_runtime(&s.runtime_cfg);
    if (s_twin_cb) {
        s_twin_cb(&s.runtime_cfg);
    }
    cJSON_Delete(root);
}

static void handle_direct_method(const char *topic, int topic_len, const char *data, int data_len)
{
    char method[64];
    char rid[32];
    char payload[1024];
    char response[512];
    int status = 200;

    extract_method_name(topic, topic_len, method, sizeof(method));
    extract_rid(topic, topic_len, rid, sizeof(rid));

    int copy = data_len < (int)(sizeof(payload) - 1) ? data_len : (int)(sizeof(payload) - 1);
    memcpy(payload, data, copy);
    payload[copy] = '\0';

    if (strcmp(method, "reboot") == 0) {
        send_method_response(rid, 200, "{\"response\":\"Rebooting\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    if (strcmp(method, "ota") == 0) {
        cJSON *root = cJSON_ParseWithLength(payload, copy);
        cJSON *url = root ? cJSON_GetObjectItem(root, "url") : NULL;
        cJSON *ver = root ? cJSON_GetObjectItem(root, "version") : NULL;
        if (!cJSON_IsString(url)) {
            send_method_response(rid, 400, "{\"response\":\"Missing url\"}");
        } else {
            esp_err_t err = ota_start(url->valuestring, cJSON_IsString(ver) ? ver->valuestring : "");
            if (err == ESP_OK) {
                send_method_response(rid, 200, "{\"response\":\"OTA started\"}");
            } else if (err == OTA_ERR_NOT_NEWER) {
                send_method_response(rid, 409, "{\"response\":\"Requested version is not newer\"}");
            } else {
                send_method_response(rid, 500, "{\"response\":\"OTA start failed\"}");
            }
        }
        if (root) cJSON_Delete(root);
        return;
    }

    if (strcmp(method, "spiffs_health") == 0) {
        storage_spiffs_health_t h = storage_spiffs_health();
        snprintf(response, sizeof(response),
                 "{\"status\":\"%s\",\"total_kb\":%u,\"used_kb\":%u,\"free_kb\":%u}",
                 h.err == ESP_OK ? "ok" : "fail",
                 (unsigned)(h.total_bytes / 1024),
                 (unsigned)(h.used_bytes / 1024),
                 (unsigned)((h.total_bytes - h.used_bytes) / 1024));
        send_method_response(rid, h.err == ESP_OK ? 200 : 500, response);
        return;
    }

    if (strcmp(method, "sd_health") == 0) {
        storage_sd_health_t h = storage_sd_health();
        snprintf(response, sizeof(response),
                 "{\"status\":\"%s\",\"write\":%s,\"read\":%s,\"match\":%s}",
                 h.err == ESP_OK ? "ok" : "fail",
                 h.write_ok ? "true" : "false",
                 h.read_ok ? "true" : "false",
                 h.match_ok ? "true" : "false");
        send_method_response(rid, h.err == ESP_OK ? 200 : 500, response);
        return;
    }

    if (strcmp(method, "read_sd_files") == 0) {
        esp_err_t err = build_sd_file_list_json(response, sizeof(response));
        send_method_response(rid, err == ESP_OK ? 200 : 500, response);
        return;
    }

    if (strcmp(method, "clear_sd") == 0) {
        bool preserve_config = true;
        cJSON *root = cJSON_ParseWithLength(payload, copy);
        if (root) {
            cJSON *item = cJSON_GetObjectItem(root, "preserve_config");
            if (cJSON_IsBool(item)) {
                preserve_config = cJSON_IsTrue(item);
            }
            cJSON_Delete(root);
        }
        int removed = 0;
        esp_err_t err = clear_sd_root(preserve_config, &removed);
        snprintf(response, sizeof(response),
                 "{\"response\":\"%s\",\"removed\":%d,\"preserve_config\":%s}",
                 err == ESP_OK ? "sd_cleared" : "sd_clear_failed",
                 removed,
                 preserve_config ? "true" : "false");
        send_method_response(rid, err == ESP_OK ? 200 : 500, response);
        return;
    }

    if (strcmp(method, "wifi_credentials") == 0) {
        cJSON *root = cJSON_ParseWithLength(payload, copy);
        cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
        cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : NULL;
        if (cJSON_IsString(ssid) && cJSON_IsString(pass) &&
            wifi_mgr_save_credentials(ssid->valuestring, pass->valuestring) == ESP_OK) {
            send_method_response(rid, 200, "{\"response\":\"Wi-Fi credentials saved\"}");
        } else {
            send_method_response(rid, 400, "{\"response\":\"Invalid Wi-Fi payload\"}");
        }
        if (root) cJSON_Delete(root);
        return;
    }

    if (strcmp(method, "azure_credentials") == 0) {
        cJSON *root = cJSON_ParseWithLength(payload, copy);
        app_config_azure_t cfg = {0};
        cJSON *host = root ? cJSON_GetObjectItem(root, "hostname") : NULL;
        cJSON *dev = root ? cJSON_GetObjectItem(root, "device_id") : NULL;
        cJSON *sak = root ? cJSON_GetObjectItem(root, "shared_access_key") : NULL;
        if (cJSON_IsString(host) && cJSON_IsString(dev) && cJSON_IsString(sak)) {
            snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", host->valuestring);
            snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", dev->valuestring);
            snprintf(cfg.shared_access_key, sizeof(cfg.shared_access_key), "%s", sak->valuestring);
            app_config_save_azure(&cfg);
            send_method_response(rid, 200, "{\"response\":\"Azure credentials saved\"}");
        } else {
            send_method_response(rid, 400, "{\"response\":\"Invalid Azure payload\"}");
        }
        if (root) cJSON_Delete(root);
        return;
    }

    if (s_method_cb && s_method_cb(method, payload, response, sizeof(response), &status) == ESP_OK) {
        send_method_response(rid, status, response);
        return;
    }

    send_method_response(rid, 404, "{\"response\":\"Method not handled\"}");
}

static void mqtt_event_bridge(const mqtt_mgr_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == MQTT_MGR_EVENT_CONNECTED) {
        s.connected = true;
        ESP_LOGI(TAG, "MQTT connected: device_id=%s device_name=%s",
                 s.creds.device_id,
                 s.runtime_cfg.device_name[0] ? s.runtime_cfg.device_name : "<unset>");
        mqtt_mgr_subscribe("$iothub/twin/res/#", 1);
        mqtt_mgr_subscribe("$iothub/twin/PATCH/properties/desired/#", 1);
        mqtt_mgr_subscribe("$iothub/methods/POST/#", 1);
        return;
    }
    if (event->type == MQTT_MGR_EVENT_DISCONNECTED) {
        s.connected = false;
        return;
    }
    if (event->type != MQTT_MGR_EVENT_DATA || !event->topic) {
        return;
    }

    if (event->topic_len >= 13 && strncmp(event->topic, "$iothub/twin/", 13) == 0) {
        handle_twin_message(event->data, event->data_len);
    } else if (event->topic_len >= 21 && strncmp(event->topic, "$iothub/methods/POST/", 21) == 0) {
        handle_direct_method(event->topic, event->topic_len, event->data, event->data_len);
    }
}

static void background_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!wifi_mgr_is_connected()) {
            continue;
        }
        if (sas_token_is_expired(s.sas_token)) {
            ESP_LOGW(TAG, "Azure SAS token expired, reconnecting");
            azure_iot_disconnect();
            azure_iot_connect();
        }
    }
}

esp_err_t azure_iot_init(const app_config_azure_t *creds, const app_runtime_config_t *runtime_cfg)
{
    if (!creds || !runtime_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.creds = *creds;
    s.runtime_cfg = *runtime_cfg;
    snprintf(s.mqtt_uri, sizeof(s.mqtt_uri), "mqtts://%s:8883", s.creds.hostname);
    snprintf(s.mqtt_username, sizeof(s.mqtt_username),
             "%s/%s/?api-version=2021-04-12", s.creds.hostname, s.creds.device_id);
    snprintf(s.telemetry_topic, sizeof(s.telemetry_topic),
             "devices/%s/messages/events/", s.creds.device_id);
    ESP_LOGI(TAG, "Azure init: device_id=%s device_name=%s host=%s",
             s.creds.device_id,
             s.runtime_cfg.device_name[0] ? s.runtime_cfg.device_name : "<unset>",
             s.creds.hostname);
    return ESP_OK;
}

void azure_iot_register_twin_callback(azure_iot_twin_cb_t cb)
{
    s_twin_cb = cb;
}

void azure_iot_register_method_callback(azure_iot_method_cb_t cb)
{
    s_method_cb = cb;
}

esp_err_t azure_iot_connect(void)
{
    uint64_t expiry = (uint64_t)time(NULL) + SAS_DURATION_SECS;
    if (sas_token_generate(s.creds.hostname, s.creds.device_id, s.creds.shared_access_key,
                           expiry, s.sas_token, sizeof(s.sas_token)) != 0) {
        return ESP_FAIL;
    }

    mqtt_mgr_config_t cfg = {0};
    snprintf(cfg.broker_uri, sizeof(cfg.broker_uri), "%s", s.mqtt_uri);
    snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", s.creds.device_id);
    snprintf(cfg.username, sizeof(cfg.username), "%s", s.mqtt_username);
    snprintf(cfg.password, sizeof(cfg.password), "%s", s.sas_token);

    ESP_ERROR_CHECK(mqtt_mgr_init(&cfg, mqtt_event_bridge, NULL));
    return mqtt_mgr_connect();
}

esp_err_t azure_iot_disconnect(void)
{
    s.connected = false;
    return mqtt_mgr_disconnect();
}

bool azure_iot_is_connected(void)
{
    return s.connected && mqtt_mgr_is_connected();
}

esp_err_t azure_iot_request_twin(void)
{
    return mqtt_mgr_publish("$iothub/twin/GET/?$rid=1", "", 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t azure_iot_publish_telemetry(const char *payload_json)
{
    if (!payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    char payload[1536];
    cJSON *root = cJSON_Parse(payload_json);
    if (root) {
        if (s.runtime_cfg.ro_code[0] != '\0') {
            cJSON_AddStringToObject(root, "ro_code", s.runtime_cfg.ro_code);
        }
        if (s.runtime_cfg.device_name[0] != '\0') {
            cJSON_AddStringToObject(root, "device_name", s.runtime_cfg.device_name);
        }
        if (!cJSON_PrintPreallocated(root, payload, sizeof(payload), false)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON_Delete(root);
    } else {
        snprintf(payload, sizeof(payload), "%s", payload_json);
    }

    return mqtt_mgr_publish(s.telemetry_topic, payload, 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t azure_iot_start_background_task(void)
{
    if (s_bg_task) {
        return ESP_OK;
    }
    if (xTaskCreate(background_task, "azure_bg", 4096, NULL, 3, &s_bg_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void azure_iot_set_runtime_config(const app_runtime_config_t *cfg)
{
    if (cfg) {
        s.runtime_cfg = *cfg;
    }
}
