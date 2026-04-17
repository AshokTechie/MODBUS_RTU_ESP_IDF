#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define NVS_MANAGER_MAX_KEY_LEN 15
#define NVS_MANAGER_MAX_NAMESPACE_LEN 15
#define NVS_MANAGER_MAX_STR_LEN 384

esp_err_t nvs_manager_init_namespace(const char *ns);
esp_err_t nvs_manager_set_str(const char *ns, const char *key, const char *value);
esp_err_t nvs_manager_get_str(const char *ns, const char *key, char *out, size_t out_size);
esp_err_t nvs_manager_set_i32(const char *ns, const char *key, int32_t value);
esp_err_t nvs_manager_get_i32(const char *ns, const char *key, int32_t *out);
esp_err_t nvs_manager_set_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t nvs_manager_get_blob(const char *ns, const char *key, void *out, size_t *len);
