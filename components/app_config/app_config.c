#include "app_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"
#include "storage_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_config";

#define APP_CONFIG_NVS_NAMESPACE "smartload"
#define APP_CONFIG_MAX_NVS_KEY_LEN 15
#define APP_CONFIG_MAX_NVS_STR_LEN 384

static esp_err_t parse_azure_json(const char *json, size_t json_len, app_config_azure_t *out_cfg);
static esp_err_t scan_mount_for_azure(const char *mount, app_config_azure_t *out_cfg);

#define MQTT_CONFIG_FILE   "/mqtt_config.json"
#define AZURE_CONFIG_FILE  "/azure_config.json"
#define LEGACY_CONFIG_FILE "/config.json"
#define RUNTIME_CONFIG_FILE "/runtime_config.json"
#define HTTP_CONFIG_FILE    "/http_config.json"
#define SMARTLOAD_HTTP_CONFIG_FILE "/smartload_http.json"

static bool app_config_valid_nvs_key(const char *key)
{
    return key && key[0] != '\0' && strlen(key) <= APP_CONFIG_MAX_NVS_KEY_LEN;
}

static esp_err_t app_config_nvs_set_str(nvs_handle_t nvs, const char *key, const char *value)
{
    if (!app_config_valid_nvs_key(key)) {
        ESP_LOGE(TAG, "Invalid NVS key '%s'", key ? key : "<null>");
        return ESP_ERR_INVALID_ARG;
    }
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= APP_CONFIG_MAX_NVS_STR_LEN) {
        ESP_LOGE(TAG, "NVS value too large for key '%s' (%u bytes)",
                 key, (unsigned)strlen(value));
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_str(nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t app_config_nvs_get_str(nvs_handle_t nvs, const char *key, char *out, size_t out_size)
{
    if (!app_config_valid_nvs_key(key) || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

static esp_err_t app_config_nvs_set_i32(nvs_handle_t nvs, const char *key, int32_t value)
{
    if (!app_config_valid_nvs_key(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_set_i32(nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t read_json_from_mount(const char *mount, const char *path, char *buf, size_t buf_size, size_t *out_len)
{
    if (!mount || !path || !buf || buf_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[160];
    snprintf(full_path, sizeof(full_path), "%s%s", mount, path);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t read = fread(buf, 1, buf_size - 1, f);
    fclose(f);
    buf[read] = '\0';
    if (out_len) {
        *out_len = read;
    }
    return ESP_OK;
}

static esp_err_t load_azure_from_path(const char *mount, const char *path, app_config_azure_t *out_cfg)
{
    char file_buf[1024] = {0};
    size_t file_len = 0;
    esp_err_t err = read_json_from_mount(mount, path, file_buf, sizeof(file_buf), &file_len);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_azure_json(file_buf, file_len, out_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Azure config loaded from %s%s (host=%s device=%s)",
                 mount, path, out_cfg->hostname, out_cfg->device_id);
    } else {
        ESP_LOGW(TAG, "Failed to parse %s%s: %s", mount, path, esp_err_to_name(err));
    }
    return err;
}

static void skip_utf8_bom(const char **json, size_t *json_len)
{
    if (!json || !*json || !json_len || *json_len < 3) {
        return;
    }

    const unsigned char *bytes = (const unsigned char *)*json;
    if (bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        *json += 3;
        *json_len -= 3;
    }
}

static bool extract_json_string(cJSON *root, const char *key1, const char *key2, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key1);
    if ((!item || !cJSON_IsString(item)) && key2) {
        item = cJSON_GetObjectItem(root, key2);
    }
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(out, out_size, "%s", item->valuestring);
        return true;
    }
    return false;
}

esp_err_t app_config_parse_connection_string(const char *conn_str, app_config_azure_t *out_cfg)
{
    const char *host = strstr(conn_str, "HostName=");
    const char *dev = strstr(conn_str, "DeviceId=");
    const char *sak = strstr(conn_str, "SharedAccessKey=");
    if (!host || !dev || !sak) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    host += strlen("HostName=");
    dev += strlen("DeviceId=");
    sak += strlen("SharedAccessKey=");

    const char *host_end = strchr(host, ';');
    const char *dev_end = strchr(dev, ';');
    const char *sak_end = strchr(sak, ';');

    size_t host_len = host_end ? (size_t)(host_end - host) : strlen(host);
    size_t dev_len = dev_end ? (size_t)(dev_end - dev) : strlen(dev);
    size_t sak_len = sak_end ? (size_t)(sak_end - sak) : strlen(sak);

    if (host_len == 0 || dev_len == 0 || sak_len == 0 ||
        host_len >= sizeof(out_cfg->hostname) ||
        dev_len >= sizeof(out_cfg->device_id) ||
        sak_len >= sizeof(out_cfg->shared_access_key)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_cfg->hostname, host, host_len);
    out_cfg->hostname[host_len] = '\0';
    memcpy(out_cfg->device_id, dev, dev_len);
    out_cfg->device_id[dev_len] = '\0';
    memcpy(out_cfg->shared_access_key, sak, sak_len);
    out_cfg->shared_access_key[sak_len] = '\0';
    return ESP_OK;
}

static esp_err_t parse_azure_json(const char *json, size_t json_len, app_config_azure_t *out_cfg)
{
    skip_utf8_bom(&json, &json_len);

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *azure = cJSON_GetObjectItem(root, "azure");
    cJSON *work = cJSON_IsObject(azure) ? azure : root;
    cJSON *conn = cJSON_GetObjectItem(work, "connectionString");
    if (!conn) {
        conn = cJSON_GetObjectItem(work, "connection_string");
    }
    if (cJSON_IsString(conn) && conn->valuestring[0] != '\0') {
        cJSON_Delete(root);
        return app_config_parse_connection_string(conn->valuestring, out_cfg);
    }

    if (!extract_json_string(work, "hostname", "HostName", out_cfg->hostname, sizeof(out_cfg->hostname))) {
        extract_json_string(work, "host_name", "hostName", out_cfg->hostname, sizeof(out_cfg->hostname));
    }
    if (!extract_json_string(work, "device_id", "DeviceId", out_cfg->device_id, sizeof(out_cfg->device_id))) {
        extract_json_string(work, "deviceId", NULL, out_cfg->device_id, sizeof(out_cfg->device_id));
    }
    if (!extract_json_string(work, "shared_access_key", "SharedAccessKey", out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key))) {
        extract_json_string(work, "sharedAccessKey", NULL, out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key));
    }
    cJSON_Delete(root);

    return (out_cfg->hostname[0] != '\0' && out_cfg->device_id[0] != '\0' && out_cfg->shared_access_key[0] != '\0')
               ? ESP_OK
               : ESP_ERR_NOT_FOUND;
}

static esp_err_t save_json_blob(const char *path, const char *json)
{
    return storage_manager_write_text(path, json);
}

static esp_err_t parse_http_json_blob(const char *json, size_t json_len, app_http_config_t *out_cfg)
{
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *http = cJSON_GetObjectItem(root, "http");
    if (!http) {
        http = cJSON_GetObjectItem(root, "smartload_http");
    }
    cJSON *work = cJSON_IsObject(http) ? http : root;

    cJSON *item = cJSON_GetObjectItem(work, "enabled");
    if (cJSON_IsBool(item)) out_cfg->enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(work, "post_url");
    if (cJSON_IsString(item)) snprintf(out_cfg->post_url, sizeof(out_cfg->post_url), "%s", item->valuestring);
    item = cJSON_GetObjectItem(work, "config_url");
    if (cJSON_IsString(item)) snprintf(out_cfg->config_url, sizeof(out_cfg->config_url), "%s", item->valuestring);
    item = cJSON_GetObjectItem(work, "conn_url");
    if (cJSON_IsString(item)) snprintf(out_cfg->conn_url, sizeof(out_cfg->conn_url), "%s", item->valuestring);
    item = cJSON_GetObjectItem(work, "post_interval_sec");
    if (cJSON_IsNumber(item)) out_cfg->post_interval_sec = (int)item->valuedouble;
    item = cJSON_GetObjectItem(work, "config_interval_sec");
    if (cJSON_IsNumber(item)) out_cfg->config_interval_sec = (int)item->valuedouble;
    item = cJSON_GetObjectItem(work, "conn_interval_sec");
    if (cJSON_IsNumber(item)) out_cfg->conn_interval_sec = (int)item->valuedouble;
    item = cJSON_GetObjectItem(work, "retry_max");
    if (cJSON_IsNumber(item)) out_cfg->retry_max = (int)item->valuedouble;
    item = cJSON_GetObjectItem(work, "retry_backoff_ms");
    if (cJSON_IsNumber(item)) out_cfg->retry_backoff_ms = (int)item->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

static void save_azure_to_nvs(const app_config_azure_t *cfg)
{
    if (!cfg) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (app_config_nvs_set_str(nvs, "az_host", cfg->hostname) == ESP_OK &&
            app_config_nvs_set_str(nvs, "az_dev", cfg->device_id) == ESP_OK &&
            app_config_nvs_set_str(nvs, "az_sak", cfg->shared_access_key) == ESP_OK) {
            esp_err_t commit_err = nvs_commit(nvs);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(azure) failed: %s", esp_err_to_name(commit_err));
            }
        }
        nvs_close(nvs);
    }
}

esp_err_t app_config_load_azure(app_config_azure_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (storage_sd_is_available()) {
        err = load_azure_from_path(STORAGE_SD_MOUNT, MQTT_CONFIG_FILE, out_cfg);
        if (err == ESP_OK) {
            save_azure_to_nvs(out_cfg);
            return ESP_OK;
        }

        err = load_azure_from_path(STORAGE_SD_MOUNT, AZURE_CONFIG_FILE, out_cfg);
        if (err == ESP_OK) {
            save_azure_to_nvs(out_cfg);
            return ESP_OK;
        }

        err = load_azure_from_path(STORAGE_SD_MOUNT, LEGACY_CONFIG_FILE, out_cfg);
        if (err == ESP_OK) {
            save_azure_to_nvs(out_cfg);
            return ESP_OK;
        }

        err = scan_mount_for_azure(STORAGE_SD_MOUNT, out_cfg);
        if (err == ESP_OK) {
            save_azure_to_nvs(out_cfg);
            return ESP_OK;
        }
    }

    err = load_azure_from_path(STORAGE_SPIFFS_MOUNT, MQTT_CONFIG_FILE, out_cfg);
    if (err == ESP_OK) {
        save_azure_to_nvs(out_cfg);
        return ESP_OK;
    }

    err = load_azure_from_path(STORAGE_SPIFFS_MOUNT, AZURE_CONFIG_FILE, out_cfg);
    if (err == ESP_OK) {
        save_azure_to_nvs(out_cfg);
        return ESP_OK;
    }

    err = load_azure_from_path(STORAGE_SPIFFS_MOUNT, LEGACY_CONFIG_FILE, out_cfg);
    if (err == ESP_OK) {
        save_azure_to_nvs(out_cfg);
        return ESP_OK;
    }

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        app_config_nvs_get_str(nvs, "az_host", out_cfg->hostname, sizeof(out_cfg->hostname));
        app_config_nvs_get_str(nvs, "az_dev", out_cfg->device_id, sizeof(out_cfg->device_id));
        app_config_nvs_get_str(nvs, "az_sak", out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key));
        nvs_close(nvs);
        if (out_cfg->hostname[0] != '\0' && out_cfg->device_id[0] != '\0') {
            ESP_LOGI(TAG, "Azure config loaded from NVS (host=%s device=%s)",
                     out_cfg->hostname, out_cfg->device_id);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Azure config not found in NVS, %s, %s, or %s",
             MQTT_CONFIG_FILE, AZURE_CONFIG_FILE, LEGACY_CONFIG_FILE);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_config_save_azure(const app_config_azure_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (app_config_nvs_set_str(nvs, "az_host", cfg->hostname) == ESP_OK &&
            app_config_nvs_set_str(nvs, "az_dev", cfg->device_id) == ESP_OK &&
            app_config_nvs_set_str(nvs, "az_sak", cfg->shared_access_key) == ESP_OK) {
            esp_err_t commit_err = nvs_commit(nvs);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(app_config_save_azure) failed: %s", esp_err_to_name(commit_err));
            }
        }
        nvs_close(nvs);
    }

    char json[512];
    snprintf(json, sizeof(json),
             "{\"hostname\":\"%s\",\"device_id\":\"%s\",\"shared_access_key\":\"%s\"}",
             cfg->hostname, cfg->device_id, cfg->shared_access_key);
    esp_err_t err = save_json_blob(MQTT_CONFIG_FILE, json);
    if (err == ESP_OK) {
        save_json_blob(AZURE_CONFIG_FILE, json);
    }
    return err;
}

static bool has_json_like_extension(const char *name)
{
    if (!name) {
        return false;
    }

    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }

    return strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".jso") == 0;
}

static esp_err_t scan_mount_for_azure(const char *mount, app_config_azure_t *out_cfg)
{
    DIR *dir = opendir(mount);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!has_json_like_extension(entry->d_name)) {
            continue;
        }

        char path[160];
        size_t name_len = strnlen(entry->d_name, sizeof(path));
        if (name_len + 2 > sizeof(path)) {
            continue;
        }
        path[0] = '/';
        memcpy(&path[1], entry->d_name, name_len);
        path[name_len + 1] = '\0';
        esp_err_t err = load_azure_from_path(mount, path, out_cfg);
        if (err == ESP_OK) {
            closedir(dir);
            return ESP_OK;
        }
    }

    closedir(dir);
    return ESP_ERR_NOT_FOUND;
}

bool app_config_validate_azure(const app_config_azure_t *cfg)
{
    if (!cfg) {
        return false;
    }

    return cfg->hostname[0] != '\0' &&
           cfg->device_id[0] != '\0' &&
           cfg->shared_access_key[0] != '\0';
}

esp_err_t app_config_load_runtime(app_runtime_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->telemetry_interval_sec = 30;
    out_cfg->polling_interval_sec = 30;

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        app_config_nvs_get_str(nvs, "ro_code", out_cfg->ro_code, sizeof(out_cfg->ro_code));
        app_config_nvs_get_str(nvs, "dev_name", out_cfg->device_name, sizeof(out_cfg->device_name));
        app_config_nvs_get_str(nvs, "dev_parm", out_cfg->device_params_json, sizeof(out_cfg->device_params_json));
        int32_t interval = 30;
        if (nvs_get_i32(nvs, "telemetry", &interval) == ESP_OK) {
            out_cfg->telemetry_interval_sec = interval;
        }
        interval = 30;
        if (nvs_get_i32(nvs, "poll_int", &interval) == ESP_OK) {
            out_cfg->polling_interval_sec = interval;
        }
        nvs_close(nvs);
        if (out_cfg->ro_code[0] != '\0' || out_cfg->device_name[0] != '\0' ||
            out_cfg->device_params_json[0] != '\0') {
            return ESP_OK;
        }
    }

    char file_buf[512] = {0};
    size_t file_len = 0;
    if (storage_manager_read_text(RUNTIME_CONFIG_FILE, file_buf, sizeof(file_buf), &file_len) != ESP_OK) {
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(file_buf, file_len);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *ro = cJSON_GetObjectItem(root, "ro_code");
    cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    cJSON *ti = cJSON_GetObjectItem(root, "telemetry_interval_sec");
    cJSON *pi = cJSON_GetObjectItem(root, "polling_interval_sec");
    cJSON *dp = cJSON_GetObjectItem(root, "device_params");
    if (cJSON_IsString(ro)) snprintf(out_cfg->ro_code, sizeof(out_cfg->ro_code), "%s", ro->valuestring);
    if (cJSON_IsString(dn)) snprintf(out_cfg->device_name, sizeof(out_cfg->device_name), "%s", dn->valuestring);
    if (cJSON_IsNumber(ti)) out_cfg->telemetry_interval_sec = (int)ti->valuedouble;
    if (cJSON_IsNumber(pi)) out_cfg->polling_interval_sec = (int)pi->valuedouble;
    if (dp && cJSON_IsObject(dp)) {
        cJSON_PrintPreallocated(dp, out_cfg->device_params_json, sizeof(out_cfg->device_params_json), false);
    } else if (cJSON_IsString(dp)) {
        snprintf(out_cfg->device_params_json, sizeof(out_cfg->device_params_json), "%s", dp->valuestring);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_config_save_runtime(const app_runtime_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (app_config_nvs_set_str(nvs, "ro_code", cfg->ro_code) == ESP_OK &&
            app_config_nvs_set_str(nvs, "dev_name", cfg->device_name) == ESP_OK &&
            app_config_nvs_set_str(nvs, "dev_parm", cfg->device_params_json) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "telemetry", cfg->telemetry_interval_sec) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "poll_int", cfg->polling_interval_sec) == ESP_OK) {
            esp_err_t commit_err = nvs_commit(nvs);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(runtime) failed: %s", esp_err_to_name(commit_err));
            }
        }
        nvs_close(nvs);
    }

    char json[512];
    snprintf(json, sizeof(json),
             "{\"ro_code\":\"%s\",\"device_name\":\"%s\",\"telemetry_interval_sec\":%d,"
             "\"polling_interval_sec\":%d,\"device_params\":%s}",
             cfg->ro_code,
             cfg->device_name,
             cfg->telemetry_interval_sec,
             cfg->polling_interval_sec,
             cfg->device_params_json[0] ? cfg->device_params_json : "{}");
    return save_json_blob(RUNTIME_CONFIG_FILE, json);
}

esp_err_t app_config_load_http(app_http_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->enabled = true;
    out_cfg->post_interval_sec = 30;
    out_cfg->config_interval_sec = 300;
    out_cfg->conn_interval_sec = 600;
    out_cfg->retry_max = 3;
    out_cfg->retry_backoff_ms = 2000;

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t value = 0;
        app_config_nvs_get_str(nvs, "h_post", out_cfg->post_url, sizeof(out_cfg->post_url));
        app_config_nvs_get_str(nvs, "h_cfg", out_cfg->config_url, sizeof(out_cfg->config_url));
        app_config_nvs_get_str(nvs, "h_conn", out_cfg->conn_url, sizeof(out_cfg->conn_url));
        if (nvs_get_i32(nvs, "http_on", &value) == ESP_OK) out_cfg->enabled = value != 0;
        if (nvs_get_i32(nvs, "h_post_i", &value) == ESP_OK) out_cfg->post_interval_sec = value;
        if (nvs_get_i32(nvs, "h_cfg_i", &value) == ESP_OK) out_cfg->config_interval_sec = value;
        if (nvs_get_i32(nvs, "h_conn_i", &value) == ESP_OK) out_cfg->conn_interval_sec = value;
        if (nvs_get_i32(nvs, "h_retry", &value) == ESP_OK) out_cfg->retry_max = value;
        if (nvs_get_i32(nvs, "h_backoff", &value) == ESP_OK) out_cfg->retry_backoff_ms = value;
        nvs_close(nvs);
        if (out_cfg->post_url[0] != '\0' || out_cfg->config_url[0] != '\0' || out_cfg->conn_url[0] != '\0') {
            return ESP_OK;
        }
    }

    char file_buf[768] = {0};
    size_t file_len = 0;
    if (storage_manager_read_text(HTTP_CONFIG_FILE, file_buf, sizeof(file_buf), &file_len) != ESP_OK) {
        if (storage_manager_read_text(SMARTLOAD_HTTP_CONFIG_FILE, file_buf, sizeof(file_buf), &file_len) != ESP_OK) {
            if (storage_manager_read_text(LEGACY_CONFIG_FILE, file_buf, sizeof(file_buf), &file_len) != ESP_OK) {
                return ESP_OK;
            }
        }
    }
    return parse_http_json_blob(file_buf, file_len, out_cfg);
}

esp_err_t app_config_save_http(const app_http_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (app_config_nvs_set_str(nvs, "h_post", cfg->post_url) == ESP_OK &&
            app_config_nvs_set_str(nvs, "h_cfg", cfg->config_url) == ESP_OK &&
            app_config_nvs_set_str(nvs, "h_conn", cfg->conn_url) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "http_on", cfg->enabled ? 1 : 0) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "h_post_i", cfg->post_interval_sec) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "h_cfg_i", cfg->config_interval_sec) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "h_conn_i", cfg->conn_interval_sec) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "h_retry", cfg->retry_max) == ESP_OK &&
            app_config_nvs_set_i32(nvs, "h_backoff", cfg->retry_backoff_ms) == ESP_OK) {
            esp_err_t commit_err = nvs_commit(nvs);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(http) failed: %s", esp_err_to_name(commit_err));
            }
        }
        nvs_close(nvs);
    }

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"enabled\":%s,\"post_url\":\"%s\",\"config_url\":\"%s\",\"conn_url\":\"%s\","
             "\"post_interval_sec\":%d,\"config_interval_sec\":%d,\"conn_interval_sec\":%d,"
             "\"retry_max\":%d,\"retry_backoff_ms\":%d}",
             cfg->enabled ? "true" : "false",
             cfg->post_url,
             cfg->config_url,
             cfg->conn_url,
             cfg->post_interval_sec,
             cfg->config_interval_sec,
             cfg->conn_interval_sec,
             cfg->retry_max,
             cfg->retry_backoff_ms);
    return save_json_blob(HTTP_CONFIG_FILE, json);
}
