#pragma once

#include "app_config.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

typedef esp_err_t (*http_client_payload_cb_t)(char *out, size_t out_size);
typedef esp_err_t (*http_client_runtime_get_cb_t)(app_runtime_config_t *out_cfg);
typedef esp_err_t (*http_client_runtime_apply_cb_t)(const app_runtime_config_t *cfg);
typedef void (*http_client_conn_update_cb_t)(const app_config_azure_t *cfg);

typedef struct {
    http_client_payload_cb_t build_payload;
    http_client_runtime_get_cb_t get_runtime_config;
    http_client_runtime_apply_cb_t apply_runtime_config;
    http_client_conn_update_cb_t on_connection_string;
} http_client_callbacks_t;

esp_err_t http_client_init(const app_http_config_t *cfg, const http_client_callbacks_t *callbacks);
esp_err_t http_client_set_config(const app_http_config_t *cfg);
esp_err_t http_client_start(void);
bool http_client_is_running(void);
