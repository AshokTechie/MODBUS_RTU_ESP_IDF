#pragma once

#include "app_config.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef void (*azure_iot_twin_cb_t)(const app_runtime_config_t *cfg);
typedef esp_err_t (*azure_iot_method_cb_t)(const char *method,
                                           const char *payload_json,
                                           char *response_json,
                                           size_t response_json_size,
                                           int *status_code);

esp_err_t azure_iot_init(const app_config_azure_t *creds, const app_runtime_config_t *runtime_cfg);
void azure_iot_register_twin_callback(azure_iot_twin_cb_t cb);
void azure_iot_register_method_callback(azure_iot_method_cb_t cb);
esp_err_t azure_iot_connect(void);
esp_err_t azure_iot_disconnect(void);
bool azure_iot_is_connected(void);
esp_err_t azure_iot_wait_until_connected(uint32_t timeout_ms);
esp_err_t azure_iot_request_twin(void);
esp_err_t azure_iot_publish_telemetry(const char *payload_json);
esp_err_t azure_iot_start_background_task(void);
void azure_iot_set_runtime_config(const app_runtime_config_t *cfg);
