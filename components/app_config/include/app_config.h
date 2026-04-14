#pragma once

#include "esp_err.h"

#define APP_CONFIG_STR_LEN 128

typedef struct {
    char hostname[APP_CONFIG_STR_LEN];
    char device_id[APP_CONFIG_STR_LEN];
    char shared_access_key[APP_CONFIG_STR_LEN];
} app_config_azure_t;

typedef struct {
    char ro_code[32];
    char device_name[64];
    int telemetry_interval_sec;
} app_runtime_config_t;

esp_err_t app_config_load_azure(app_config_azure_t *out_cfg);
esp_err_t app_config_save_azure(const app_config_azure_t *cfg);
esp_err_t app_config_load_runtime(app_runtime_config_t *out_cfg);
esp_err_t app_config_save_runtime(const app_runtime_config_t *cfg);
