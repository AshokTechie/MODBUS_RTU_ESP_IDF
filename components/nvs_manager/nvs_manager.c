#include "nvs_manager.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "nvs_manager";

static bool nvs_manager_valid_key(const char *key)
{
    return key && key[0] != '\0' && strlen(key) <= NVS_MANAGER_MAX_KEY_LEN;
}

static esp_err_t nvs_manager_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!ns || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(ns, mode, out);
}

esp_err_t nvs_manager_set_str(const char *ns, const char *key, const char *value)
{
    if (!nvs_manager_valid_key(key) || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= NVS_MANAGER_MAX_STR_LEN) {
        ESP_LOGE(TAG, "Value too large for key %s", key);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t nvs_manager_get_str(const char *ns, const char *key, char *out, size_t out_size)
{
    if (!nvs_manager_valid_key(key) || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    size_t len = out_size;
    err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t nvs_manager_set_i32(const char *ns, const char *key, int32_t value)
{
    if (!nvs_manager_valid_key(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t nvs_manager_get_i32(const char *ns, const char *key, int32_t *out)
{
    if (!nvs_manager_valid_key(key) || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_i32(nvs, key, out);
    nvs_close(nvs);
    return err;
}

esp_err_t nvs_manager_set_blob(const char *ns, const char *key, const void *data, size_t len)
{
    if (!nvs_manager_valid_key(key) || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t nvs_manager_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    if (!nvs_manager_valid_key(key) || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(nvs, key, out, len);
    nvs_close(nvs);
    return err;
}
