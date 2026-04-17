#include "nvs_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "nvs_manager";
static SemaphoreHandle_t s_nvs_lock = NULL;

#define NVS_MANAGER_INIT_MARKER_KEY "__init__"
#define NVS_MANAGER_INIT_MARKER_VALUE 1U

static esp_err_t nvs_manager_lock_take(void)
{
    if (!s_nvs_lock) {
        s_nvs_lock = xSemaphoreCreateMutex();
        if (!s_nvs_lock) {
            ESP_LOGE(TAG, "Failed to create NVS mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_nvs_lock, portMAX_DELAY);
    return ESP_OK;
}

static void nvs_manager_lock_give(void)
{
    if (s_nvs_lock) {
        xSemaphoreGive(s_nvs_lock);
    }
}

static bool nvs_manager_valid_namespace(const char *ns)
{
    return ns && ns[0] != '\0' && strlen(ns) <= NVS_MANAGER_MAX_NAMESPACE_LEN;
}

static bool nvs_manager_valid_key(const char *key)
{
    return key && key[0] != '\0' && strlen(key) <= NVS_MANAGER_MAX_KEY_LEN;
}

static esp_err_t nvs_manager_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!nvs_manager_valid_namespace(ns) || !out) {
        ESP_LOGE(TAG, "Invalid NVS namespace");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_open(ns, mode, out);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND && mode == NVS_READONLY) {
            ESP_LOGD(TAG, "nvs_open(%s) missing namespace", ns);
        } else {
            ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t nvs_manager_init_namespace(const char *ns)
{
    if (!nvs_manager_valid_namespace(ns)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    esp_err_t result = ESP_OK;
    uint8_t marker = 0;
    err = nvs_get_u8(nvs, NVS_MANAGER_INIT_MARKER_KEY, &marker);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_u8(nvs, NVS_MANAGER_INIT_MARKER_KEY, NVS_MANAGER_INIT_MARKER_VALUE);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize namespace %s: %s", ns, esp_err_to_name(err));
            result = err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read namespace marker for %s: %s", ns, esp_err_to_name(err));
        result = err;
    }

    nvs_close(nvs);
    nvs_manager_lock_give();
    return result;
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

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    err = nvs_set_str(nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(err));
        }
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
    return err;
}

esp_err_t nvs_manager_get_str(const char *ns, const char *key, char *out, size_t out_size)
{
    if (!nvs_manager_valid_key(key) || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        out[0] = '\0';
        nvs_manager_lock_give();
        return err;
    }

    size_t len = out_size;
    err = nvs_get_str(nvs, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
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

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    err = nvs_set_i32(nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) failed: %s", key, esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(err));
        }
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
    return err;
}

esp_err_t nvs_manager_get_i32(const char *ns, const char *key, int32_t *out)
{
    if (!nvs_manager_valid_key(key) || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    err = nvs_get_i32(nvs, key, out);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i32(%s) failed: %s", key, esp_err_to_name(err));
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
    return err;
}

esp_err_t nvs_manager_set_blob(const char *ns, const char *key, const void *data, size_t len)
{
    if (!nvs_manager_valid_key(key) || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    err = nvs_set_blob(nvs, key, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(err));
        }
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
    return err;
}

esp_err_t nvs_manager_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    if (!nvs_manager_valid_key(key) || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_lock_take();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_manager_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        nvs_manager_lock_give();
        return err;
    }

    err = nvs_get_blob(nvs, key, out, len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_blob(%s) failed: %s", key, esp_err_to_name(err));
    }
    nvs_close(nvs);
    nvs_manager_lock_give();
    return err;
}
