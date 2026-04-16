#pragma once

#include "app_config.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#define SMARTLOAD_HTTP_QUEUE_DEPTH 4
#define SMARTLOAD_HTTP_PAYLOAD_MAX 1024

typedef esp_err_t (*smartload_http_runtime_get_cb_t)(app_runtime_config_t *out_cfg);
typedef esp_err_t (*smartload_http_runtime_apply_cb_t)(const app_runtime_config_t *cfg);
typedef void (*smartload_http_conn_update_cb_t)(const app_config_azure_t *cfg);

typedef struct {
    smartload_http_runtime_get_cb_t get_runtime_config;
    smartload_http_runtime_apply_cb_t apply_runtime_config;
    smartload_http_conn_update_cb_t on_connection_string;
} smartload_http_callbacks_t;

esp_err_t smartload_http_init(const app_http_config_t *cfg, const smartload_http_callbacks_t *callbacks);
esp_err_t smartload_http_set_config(const app_http_config_t *cfg);
esp_err_t smartload_http_send(const char *payload_json);
bool smartload_http_is_running(void);
