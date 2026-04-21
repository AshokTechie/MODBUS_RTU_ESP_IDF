#include "azure_iot.h"

#include "app_config.h"
#include "board_utils.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

#include "mbedtls/base64.h"

static const char *TAG = "azure_iot";

#define SAS_DURATION_SECS (30ULL * 24ULL * 60ULL * 60ULL)
#define AZURE_CONNECT_TIMEOUT_MS 15000
#define AZURE_RECONNECT_INTERVAL_MS 10000
#define SMARTLOAD_HTTP_FALLBACK_URL "https://shiftlogs.azurewebsites.net/api/ingest_transaction"
#define SMARTLOAD_PENDING_PREFIX "/smartload_pending_"
#define SMARTLOAD_PENDING_SCAN_MAX 16
#define SMARTLOAD_PENDING_RETRY_MS 30000
#define SMARTLOAD_DRAIN_BUFFER_MAX 1536
#define AZURE_BG_TASK_STACK 8192

static struct {
    app_config_azure_t creds;
    app_runtime_config_t runtime_cfg;
    char sas_token[256];
    char mqtt_uri[192];
    char mqtt_username[320];
    char telemetry_topic[192];
    bool connected;
    volatile bool stopping;
    TickType_t last_reconnect_tick;
    TickType_t last_pending_drain_tick;
    uint32_t pending_seq;
} s;

static TaskHandle_t s_bg_task = NULL;
static azure_iot_twin_cb_t s_twin_cb = NULL;
static azure_iot_method_cb_t s_method_cb = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static void azure_iot_lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void azure_iot_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static const char *boot_board_code(void)
{
#if defined(HARDWARE_NEW_PCB)
    return "n";
#else
    return board_get_board_code();
#endif
}

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

static void build_ingest_device_id(const char *ro_code, char *out, size_t out_size)
{
    const char legacy_suffix[] = { 0x72, 0x64, 0x75, 0x00 };
    char plain[96];
    snprintf(plain, sizeof(plain), "bpcl-%s-%s", ro_code, legacy_suffix);
    size_t plain_len = strlen(plain);
    for (size_t i = 0, j = plain_len ? plain_len - 1U : 0U; i < j; i++, j--) {
        char tmp = plain[i];
        plain[i] = plain[j];
        plain[j] = tmp;
    }

    unsigned char encoded[160];
    size_t encoded_len = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                              (const unsigned char *)plain, plain_len) != 0) {
        out[0] = '\0';
        return;
    }
    encoded[encoded_len] = '\0';
    snprintf(out, out_size, "%s", (const char *)encoded);
}

static const char *resolve_http_post_url(char *out, size_t out_size)
{
    app_http_config_t http_cfg = {0};
    if (app_config_load_http(&http_cfg) == ESP_OK && http_cfg.post_url[0] != '\0') {
        snprintf(out, out_size, "%s", http_cfg.post_url);
    } else {
        snprintf(out, out_size, "%s", SMARTLOAD_HTTP_FALLBACK_URL);
    }
    return out;
}

static esp_err_t http_post_once(const char *url, const char *payload, const char *device_id_hdr, bool insecure_retry)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = insecure_retry,
    };
    if (!insecure_retry) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (device_id_hdr && device_id_hdr[0] != '\0') {
        esp_http_client_set_header(client, "device_id", device_id_hdr);
    }
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP ingest failed (%s): %s", insecure_retry ? "insecure" : "secure", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP ingest status=%d (%s)", status, insecure_retry ? "insecure" : "secure");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t deliver_via_http(const char *payload)
{
    if (s.runtime_cfg.ro_code[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char device_id_hdr[160];
    char url[APP_CONFIG_URL_LEN];
    build_ingest_device_id(s.runtime_cfg.ro_code, device_id_hdr, sizeof(device_id_hdr));
    resolve_http_post_url(url, sizeof(url));

    esp_err_t err = http_post_once(url, payload, device_id_hdr, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS delivery failed, retrying without CN check");
        err = http_post_once(url, payload, device_id_hdr, true);
    }
    return err;
}

static void save_pending_payload(const char *payload)
{
    char path[64];
    s.pending_seq++;
    snprintf(path, sizeof(path), SMARTLOAD_PENDING_PREFIX "%08lu.json",
             (unsigned long)s.pending_seq);
    if (storage_write(path, payload) == ESP_OK) {
        ESP_LOGW(TAG, "Queued pending telemetry at %s", path);
    } else {
        ESP_LOGE(TAG, "Failed to queue pending telemetry");
    }
}

static esp_err_t deliver_payload(const char *payload)
{
    bool mqtt_ok = false;
    int mid = -1;

    /* Prevent racing with reconnect/disconnect (which can surface as EOF/errno=119). */
    azure_iot_lock();
    mqtt_ok = (s.connected && mqtt_mgr_is_connected());
    if (mqtt_ok) {
        mid = mqtt_mgr_publish(s.telemetry_topic, payload, 1, 0);
    }
    azure_iot_unlock();

    if (mqtt_ok && mid >= 0) {
        return ESP_OK;
    }
    if (mqtt_ok) {
        ESP_LOGW(TAG, "MQTT publish returned %d, falling back to HTTP", mid);
    }
    return deliver_via_http(payload);
}

static void drain_pending_payloads(void)
{
    static storage_file_info_t files[SMARTLOAD_PENDING_SCAN_MAX];
    static char payload[SMARTLOAD_DRAIN_BUFFER_MAX];
    size_t count = 0;
    if (storage_list_prefix(STORAGE_SPIFFS_MOUNT,
                            SMARTLOAD_PENDING_PREFIX,
                            files,
                            SMARTLOAD_PENDING_SCAN_MAX,
                            &count) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        size_t len = 0;
        if (storage_read(files[i].path, payload, sizeof(payload), &len) != ESP_OK) {
            continue;
        }
        if (deliver_payload(payload) == ESP_OK) {
            storage_remove(files[i].path);
            ESP_LOGI(TAG, "Drained pending telemetry %s", files[i].path);
        } else {
            ESP_LOGW(TAG, "Pending telemetry still undelivered: %s", files[i].path);
            break;
        }
    }
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
    if (s.stopping) {
        return;
    }
    if (event->type == MQTT_MGR_EVENT_CONNECTED) {
        s.connected = true;
        ESP_LOGI(TAG, "MQTT connected: device_id=%s device_name=%s",
                 s.creds.device_id,
                 s.runtime_cfg.device_name[0] ? s.runtime_cfg.device_name : "<unset>");
        mqtt_mgr_subscribe("$iothub/twin/res/#", 1);
        mqtt_mgr_subscribe("$iothub/twin/PATCH/properties/desired/#", 1);
        mqtt_mgr_subscribe("$iothub/methods/POST/#", 1);
        mqtt_mgr_publish("$iothub/twin/GET/?$rid=1", "", 1, 0);
        return;
    }
    if (event->type == MQTT_MGR_EVENT_DISCONNECTED) {
        s.connected = false;
        ESP_LOGW(TAG, "MQTT disconnected: device_id=%s", s.creds.device_id);
        return;
    }
    if (event->type == MQTT_MGR_EVENT_ERROR) {
        s.connected = false;
        ESP_LOGW(TAG, "MQTT transport error: device_id=%s", s.creds.device_id);
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
            continue;
        }
        if (!azure_iot_is_connected()) {
            TickType_t now = xTaskGetTickCount();
            if (s.last_reconnect_tick == 0 ||
                (now - s.last_reconnect_tick) >= pdMS_TO_TICKS(AZURE_RECONNECT_INTERVAL_MS)) {
                s.last_reconnect_tick = now;
                ESP_LOGW(TAG, "Azure MQTT offline, attempting reconnect");
                azure_iot_disconnect();
                azure_iot_connect();
            }
        }
        TickType_t now = xTaskGetTickCount();
        if (s.last_pending_drain_tick == 0 ||
            (now - s.last_pending_drain_tick) >= pdMS_TO_TICKS(SMARTLOAD_PENDING_RETRY_MS)) {
            s.last_pending_drain_tick = now;
            drain_pending_payloads();
        }
    }
}

esp_err_t azure_iot_init(const app_config_azure_t *creds, const app_runtime_config_t *runtime_cfg)
{
    if (!creds || !runtime_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s, 0, sizeof(s));
    s.creds = *creds;
    s.runtime_cfg = *runtime_cfg;
    snprintf(s.mqtt_uri, sizeof(s.mqtt_uri), "mqtts://%s:8883", s.creds.hostname);
    snprintf(s.mqtt_username, sizeof(s.mqtt_username),
             "%s/%s/?api-version=2021-04-12", s.creds.hostname, s.creds.device_id);
    snprintf(s.telemetry_topic, sizeof(s.telemetry_topic),
             "devices/%s/messages/events/", s.creds.device_id);

    uint64_t chip_id = board_read_chip_id();
    ESP_LOGI(TAG, "Azure init: device_id=%s device_name=%s host=%s board=%s chip_id=%llu",
             s.creds.device_id,
             s.runtime_cfg.device_name[0] ? s.runtime_cfg.device_name : "<unset>",
             s.creds.hostname,
             boot_board_code(),
             (unsigned long long)chip_id);
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
    azure_iot_lock();
    s.stopping = false;
    uint64_t expiry = (uint64_t)time(NULL) + SAS_DURATION_SECS;
    if (sas_token_generate(s.creds.hostname, s.creds.device_id, s.creds.shared_access_key,
                           expiry, s.sas_token, sizeof(s.sas_token)) != 0) {
        azure_iot_unlock();
        return ESP_FAIL;
    }

    mqtt_mgr_config_t cfg = {0};
    snprintf(cfg.broker_uri, sizeof(cfg.broker_uri), "%s", s.mqtt_uri);
    snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", s.creds.device_id);
    snprintf(cfg.username, sizeof(cfg.username), "%s", s.mqtt_username);
    snprintf(cfg.password, sizeof(cfg.password), "%s", s.sas_token);

    esp_err_t err = mqtt_mgr_init(&cfg, mqtt_event_bridge, NULL);
    if (err != ESP_OK) {
        azure_iot_unlock();
        ESP_LOGW(TAG, "mqtt_mgr_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = mqtt_mgr_connect();
    if (err != ESP_OK) {
        azure_iot_unlock();
        ESP_LOGW(TAG, "mqtt_mgr_connect failed: %s", esp_err_to_name(err));
        return err;
    }
    s.last_reconnect_tick = xTaskGetTickCount();
    azure_iot_unlock();
    return azure_iot_wait_until_connected(AZURE_CONNECT_TIMEOUT_MS);
}

esp_err_t azure_iot_disconnect(void)
{
    azure_iot_lock();
    s.connected = false;
    s.stopping = true;
    if (!mqtt_mgr_is_connected()) {
        esp_err_t err = mqtt_mgr_destroy();
        s.stopping = false;
        azure_iot_unlock();
        return err;
    }
    esp_err_t err = mqtt_mgr_disconnect();
    mqtt_mgr_destroy();
    s.stopping = false;
    azure_iot_unlock();
    return err;
}

bool azure_iot_is_connected(void)
{
    return s.connected && mqtt_mgr_is_connected();
}

esp_err_t azure_iot_wait_until_connected(uint32_t timeout_ms)
{
    esp_err_t err = mqtt_mgr_wait_until_connected(timeout_ms);
    if (err != ESP_OK) {
        s.connected = false;
        ESP_LOGW(TAG, "Azure MQTT connect wait failed: %s", esp_err_to_name(err));
        return err;
    }
    s.connected = true;
    return ESP_OK;
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

        cJSON_AddStringToObject(root, "board_code", boot_board_code());

        char chip_id_str[24];
        snprintf(chip_id_str, sizeof(chip_id_str), "%llu",
                 (unsigned long long)board_read_chip_id());
        cJSON_AddStringToObject(root, "chip_id", chip_id_str);

        if (!cJSON_PrintPreallocated(root, payload, sizeof(payload), false)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON_Delete(root);
    } else {
        snprintf(payload, sizeof(payload), "%s", payload_json);
    }

    esp_err_t err = deliver_payload(payload);
    if (err != ESP_OK) {
        save_pending_payload(payload);
    }
    return err;
}

esp_err_t azure_iot_start_background_task(void)
{
    if (s_bg_task) {
        return ESP_OK;
    }
    if (xTaskCreate(background_task, "azure_bg", AZURE_BG_TASK_STACK, NULL, 3, &s_bg_task) != pdPASS) {
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
