#include "app_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_config";

static esp_err_t parse_azure_json(const char *json, size_t json_len, app_config_azure_t *out_cfg);

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

static esp_err_t parse_connection_string(const char *conn_str, app_config_azure_t *out_cfg)
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
        return parse_connection_string(conn->valuestring, out_cfg);
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
    return storage_write(path, json);
}

esp_err_t app_config_load_azure(app_config_azure_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));

    nvs_handle_t nvs;
    if (nvs_open("smartload", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(out_cfg->hostname);
        nvs_get_str(nvs, "az_host", out_cfg->hostname, &len);
        len = sizeof(out_cfg->device_id);
        nvs_get_str(nvs, "az_dev", out_cfg->device_id, &len);
        len = sizeof(out_cfg->shared_access_key);
        nvs_get_str(nvs, "az_sak", out_cfg->shared_access_key, &len);
        nvs_close(nvs);
        if (out_cfg->hostname[0] != '\0' && out_cfg->device_id[0] != '\0') {
            ESP_LOGI(TAG, "Azure config loaded from NVS (host=%s device=%s)",
                     out_cfg->hostname, out_cfg->device_id);
            return ESP_OK;
        }
    }

    esp_err_t err = load_azure_from_path(STORAGE_SPIFFS_MOUNT, "/azure_config.json", out_cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (storage_sd_is_available()) {
        err = load_azure_from_path(STORAGE_SD_MOUNT, "/azure_config.json", out_cfg);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    err = load_azure_from_path(STORAGE_SPIFFS_MOUNT, "/config.json", out_cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (storage_sd_is_available()) {
        err = load_azure_from_path(STORAGE_SD_MOUNT, "/config.json", out_cfg);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Azure config not found in NVS, /azure_config.json, or /config.json");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_config_save_azure(const app_config_azure_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    if (nvs_open("smartload", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "az_host", cfg->hostname);
        nvs_set_str(nvs, "az_dev", cfg->device_id);
        nvs_set_str(nvs, "az_sak", cfg->shared_access_key);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    char json[512];
    snprintf(json, sizeof(json),
             "{\"hostname\":\"%s\",\"device_id\":\"%s\",\"shared_access_key\":\"%s\"}",
             cfg->hostname, cfg->device_id, cfg->shared_access_key);
    return save_json_blob("/azure_config.json", json);
}

esp_err_t app_config_load_runtime(app_runtime_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->telemetry_interval_sec = 30;

    nvs_handle_t nvs;
    if (nvs_open("smartload", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(out_cfg->ro_code);
        nvs_get_str(nvs, "ro_code", out_cfg->ro_code, &len);
        len = sizeof(out_cfg->device_name);
        nvs_get_str(nvs, "dev_name", out_cfg->device_name, &len);
        int32_t interval = 30;
        if (nvs_get_i32(nvs, "telemetry", &interval) == ESP_OK) {
            out_cfg->telemetry_interval_sec = interval;
        }
        nvs_close(nvs);
        if (out_cfg->ro_code[0] != '\0' || out_cfg->device_name[0] != '\0') {
            return ESP_OK;
        }
    }

    char file_buf[256] = {0};
    size_t file_len = 0;
    if (storage_read("/runtime_config.json", file_buf, sizeof(file_buf), &file_len) != ESP_OK) {
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(file_buf, file_len);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *ro = cJSON_GetObjectItem(root, "ro_code");
    cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    cJSON *ti = cJSON_GetObjectItem(root, "telemetry_interval_sec");
    if (cJSON_IsString(ro)) snprintf(out_cfg->ro_code, sizeof(out_cfg->ro_code), "%s", ro->valuestring);
    if (cJSON_IsString(dn)) snprintf(out_cfg->device_name, sizeof(out_cfg->device_name), "%s", dn->valuestring);
    if (cJSON_IsNumber(ti)) out_cfg->telemetry_interval_sec = (int)ti->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_config_save_runtime(const app_runtime_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    if (nvs_open("smartload", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ro_code", cfg->ro_code);
        nvs_set_str(nvs, "dev_name", cfg->device_name);
        nvs_set_i32(nvs, "telemetry", cfg->telemetry_interval_sec);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"ro_code\":\"%s\",\"device_name\":\"%s\",\"telemetry_interval_sec\":%d}",
             cfg->ro_code, cfg->device_name, cfg->telemetry_interval_sec);
    return save_json_blob("/runtime_config.json", json);
}
