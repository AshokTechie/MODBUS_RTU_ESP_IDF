#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define APP_CONFIG_STR_LEN 128
#define APP_CONFIG_URL_LEN 256
#define APP_CONFIG_JSON_LEN 256

typedef struct {
    char hostname[APP_CONFIG_STR_LEN];
    char device_id[APP_CONFIG_STR_LEN];
    char shared_access_key[APP_CONFIG_STR_LEN];
} app_config_azure_t;

typedef struct {
    char ro_code[32];
    char device_name[64];
    int telemetry_interval_sec;
    int polling_interval_sec;
    char device_params_json[APP_CONFIG_JSON_LEN];
} app_runtime_config_t;

typedef struct {
    bool enabled;
    char post_url[APP_CONFIG_URL_LEN];
    char config_url[APP_CONFIG_URL_LEN];
    char conn_url[APP_CONFIG_URL_LEN];
    int post_interval_sec;
    int config_interval_sec;
    int conn_interval_sec;
    int retry_max;
    int retry_backoff_ms;
} app_http_config_t;

esp_err_t app_config_load_azure(app_config_azure_t *out_cfg);
esp_err_t app_config_save_azure(const app_config_azure_t *cfg);
bool app_config_validate_azure(const app_config_azure_t *cfg);
esp_err_t app_config_parse_connection_string(const char *conn_str, app_config_azure_t *out_cfg);
esp_err_t app_config_load_runtime(app_runtime_config_t *out_cfg);
esp_err_t app_config_save_runtime(const app_runtime_config_t *cfg);
esp_err_t app_config_load_http(app_http_config_t *out_cfg);
esp_err_t app_config_save_http(const app_http_config_t *cfg);
