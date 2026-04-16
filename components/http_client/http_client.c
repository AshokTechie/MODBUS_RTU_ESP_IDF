#include "http_client.h"

#include "app_config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_manager.h"
#include "storage_manager.h"
#include "wifi_mgr.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "http_client";

#define HTTP_CLIENT_TASK_STACK 8192
#define HTTP_CLIENT_TASK_PRIO 4
#define HTTP_CLIENT_NS "smartload"
#define HTTP_CLIENT_KEY_CONN "az_conn"

typedef struct {
    char *buf;
    size_t buf_size;
    size_t len;
    bool overflow;
} http_resp_ctx_t;

static SemaphoreHandle_t s_lock = NULL;
static app_http_config_t s_cfg = {0};
static http_client_callbacks_t s_callbacks = {0};
static TaskHandle_t s_task = NULL;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (!ctx || evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t available = (ctx->buf_size > ctx->len) ? (ctx->buf_size - ctx->len - 1) : 0;
    size_t to_copy = (size_t)evt->data_len;
    if (to_copy > available) {
        to_copy = available;
        ctx->overflow = true;
    }
    if (to_copy > 0U) {
        memcpy(ctx->buf + ctx->len, evt->data, to_copy);
        ctx->len += to_copy;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

static void http_copy_config(app_http_config_t *out_cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_cfg = s_cfg;
    xSemaphoreGive(s_lock);
}

static int http_positive_or_default(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

static const char *http_trim_leading_ws(const char *text)
{
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static esp_err_t http_perform_json_request(esp_http_client_method_t method,
                                           const char *url,
                                           const char *payload,
                                           char *response_buf,
                                           size_t response_buf_size,
                                           int *out_status)
{
    if (!url || !response_buf || response_buf_size < 2 || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buf[0] = '\0';
    *out_status = 0;

    http_resp_ctx_t resp = {
        .buf = response_buf,
        .buf_size = response_buf_size,
        .len = 0,
        .overflow = false,
    };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .disable_auto_redirect = false,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    if (payload) {
        esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    }

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (resp.overflow) {
        ESP_LOGW(TAG, "HTTP response from %s truncated at %u bytes", url, (unsigned)resp.len);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s (%s)", url, esp_err_to_name(err));
        return err;
    }
    if (*out_status < 200 || *out_status >= 300) {
        ESP_LOGW(TAG, "HTTP request returned status=%d for %s body=%s", *out_status, url, response_buf);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http_request_with_retry(esp_http_client_method_t method,
                                         const char *url,
                                         const char *payload,
                                         const app_http_config_t *cfg,
                                         char *response_buf,
                                         size_t response_buf_size,
                                         int *out_status)
{
    int retry_max = http_positive_or_default(cfg->retry_max, 3);
    int backoff_ms = http_positive_or_default(cfg->retry_backoff_ms, 2000);

    for (int attempt = 1; attempt <= retry_max; ++attempt) {
        if (!wifi_mgr_is_connected()) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "HTTP %s attempt %d/%d -> %s",
                 method == HTTP_METHOD_POST ? "POST" : "GET",
                 attempt,
                 retry_max,
                 url);
        esp_err_t err = http_perform_json_request(method, url, payload, response_buf, response_buf_size, out_status);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP %s success status=%d <- %s body=%s",
                     method == HTTP_METHOD_POST ? "POST" : "GET",
                     *out_status,
                     url,
                     response_buf);
            return ESP_OK;
        }

        if (attempt < retry_max) {
            int delay_ms = backoff_ms * attempt;
            ESP_LOGW(TAG, "HTTP retry in %d ms for %s", delay_ms, url);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    return ESP_FAIL;
}

static bool http_copy_json_string(cJSON *root, const char *key1, const char *key2, char *out, size_t out_size)
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

static esp_err_t http_apply_runtime_json(const char *body)
{
    if (!body || !s_callbacks.apply_runtime_config) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *cfg_obj = cJSON_GetObjectItem(root, "config");
    cJSON *work = cJSON_IsObject(cfg_obj) ? cfg_obj : root;

    app_runtime_config_t next_cfg = {0};
    if (s_callbacks.get_runtime_config) {
        s_callbacks.get_runtime_config(&next_cfg);
    } else {
        app_config_load_runtime(&next_cfg);
    }

    cJSON *item = cJSON_GetObjectItem(work, "telemetry_interval_sec");
    if (cJSON_IsNumber(item)) {
        next_cfg.telemetry_interval_sec = (int)item->valuedouble;
    }

    item = cJSON_GetObjectItem(work, "polling_interval_sec");
    if (cJSON_IsNumber(item)) {
        next_cfg.polling_interval_sec = (int)item->valuedouble;
    }

    http_copy_json_string(work, "device_name", "deviceName", next_cfg.device_name, sizeof(next_cfg.device_name));
    http_copy_json_string(work, "ro_code", "roCode", next_cfg.ro_code, sizeof(next_cfg.ro_code));

    item = cJSON_GetObjectItem(work, "device_params");
    if (!item) {
        item = cJSON_GetObjectItem(work, "deviceParameters");
    }
    if (item && cJSON_IsObject(item)) {
        cJSON_PrintPreallocated(item, next_cfg.device_params_json, sizeof(next_cfg.device_params_json), false);
    } else if (item && cJSON_IsString(item)) {
        snprintf(next_cfg.device_params_json, sizeof(next_cfg.device_params_json), "%s", item->valuestring);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Applying HTTP config: poll=%d telemetry=%d device=%s",
             next_cfg.polling_interval_sec,
             next_cfg.telemetry_interval_sec,
             next_cfg.device_name);
    return s_callbacks.apply_runtime_config(&next_cfg);
}

static esp_err_t http_parse_connection_payload(const char *body,
                                               char *connection_string,
                                               size_t connection_string_size,
                                               app_config_azure_t *out_cfg)
{
    if (!body || !connection_string || connection_string_size == 0 || !out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    connection_string[0] = '\0';

    const char *trimmed = http_trim_leading_ws(body);
    if (trimmed && strncmp(trimmed, "HostName=", 9) == 0) {
        snprintf(connection_string, connection_string_size, "%s", trimmed);
        ESP_RETURN_ON_ERROR(app_config_parse_connection_string(trimmed, out_cfg), TAG, "plain conn string parse failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *conn = cJSON_GetObjectItem(root, "connectionString");
    if (!conn) {
        conn = cJSON_GetObjectItem(root, "connection_string");
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (cJSON_IsString(conn) && conn->valuestring[0] != '\0') {
        snprintf(connection_string, connection_string_size, "%s", conn->valuestring);
        err = app_config_parse_connection_string(conn->valuestring, out_cfg);
    } else {
        http_copy_json_string(root, "hostname", "HostName", out_cfg->hostname, sizeof(out_cfg->hostname));
        http_copy_json_string(root, "device_id", "DeviceId", out_cfg->device_id, sizeof(out_cfg->device_id));
        if (out_cfg->device_id[0] == '\0') {
            http_copy_json_string(root, "deviceId", NULL, out_cfg->device_id, sizeof(out_cfg->device_id));
        }
        http_copy_json_string(root, "shared_access_key", "SharedAccessKey",
                              out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key));
        if (out_cfg->shared_access_key[0] == '\0') {
            http_copy_json_string(root, "sharedAccessKey", NULL,
                                  out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key));
        }

        if (app_config_validate_azure(out_cfg)) {
            snprintf(connection_string,
                     connection_string_size,
                     "HostName=%s;DeviceId=%s;SharedAccessKey=%s",
                     out_cfg->hostname,
                     out_cfg->device_id,
                     out_cfg->shared_access_key);
            err = ESP_OK;
        }
    }

    cJSON_Delete(root);
    return err;
}

static void http_process_post(const app_http_config_t *cfg)
{
    if (!cfg->post_url[0] || !s_callbacks.build_payload) {
        return;
    }

    char payload[768];
    char response[512];
    int status = 0;
    if (s_callbacks.build_payload(payload, sizeof(payload)) != ESP_OK) {
        ESP_LOGW(TAG, "Skipping HTTP POST: no payload available");
        return;
    }

    ESP_LOGI(TAG, "HTTP POST payload => %s", payload);
    esp_err_t err = http_request_with_retry(HTTP_METHOD_POST,
                                            cfg->post_url,
                                            payload,
                                            cfg,
                                            response,
                                            sizeof(response),
                                            &status);
    if (err != ESP_OK) {
        char saved_path[64];
        storage_manager_store_http_failure(payload, saved_path, sizeof(saved_path));
        ESP_LOGW(TAG, "HTTP POST failed for %s status=%d", cfg->post_url, status);
    }
}

static void http_process_config_pull(const app_http_config_t *cfg)
{
    if (!cfg->config_url[0]) {
        return;
    }

    char response[768];
    int status = 0;
    esp_err_t err = http_request_with_retry(HTTP_METHOD_GET,
                                            cfg->config_url,
                                            NULL,
                                            cfg,
                                            response,
                                            sizeof(response),
                                            &status);
    if (err != ESP_OK) {
        return;
    }

    err = http_apply_runtime_json(response);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP config apply failed: %s", esp_err_to_name(err));
    }
}

static void http_process_connection_pull(const app_http_config_t *cfg)
{
    if (!cfg->conn_url[0]) {
        return;
    }

    char response[768];
    char connection_string[APP_CONFIG_URL_LEN + APP_CONFIG_STR_LEN];
    int status = 0;
    app_config_azure_t azure_cfg = {0};

    esp_err_t err = http_request_with_retry(HTTP_METHOD_GET,
                                            cfg->conn_url,
                                            NULL,
                                            cfg,
                                            response,
                                            sizeof(response),
                                            &status);
    if (err != ESP_OK) {
        return;
    }

    err = http_parse_connection_payload(response, connection_string, sizeof(connection_string), &azure_cfg);
    if (err != ESP_OK || !app_config_validate_azure(&azure_cfg)) {
        ESP_LOGW(TAG, "Ignoring invalid HTTP connection string payload");
        return;
    }

    err = nvs_manager_set_str(HTTP_CLIENT_NS, HTTP_CLIENT_KEY_CONN, connection_string);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to store az_conn in NVS: %s", esp_err_to_name(err));
        return;
    }

    err = app_config_save_azure(&azure_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist Azure config: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Stored Azure connection for device=%s", azure_cfg.device_id);
    if (s_callbacks.on_connection_string) {
        s_callbacks.on_connection_string(&azure_cfg);
    }
}

static void http_client_task(void *arg)
{
    (void)arg;

    TickType_t last_post = 0;
    TickType_t last_cfg = 0;
    TickType_t last_conn = 0;

    for (;;) {
        app_http_config_t cfg;
        http_copy_config(&cfg);

        if (!cfg.enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!wifi_mgr_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t post_ticks = pdMS_TO_TICKS((uint32_t)http_positive_or_default(cfg.post_interval_sec, 30) * 1000U);
        TickType_t cfg_ticks = pdMS_TO_TICKS((uint32_t)http_positive_or_default(cfg.config_interval_sec, 300) * 1000U);
        TickType_t conn_ticks = pdMS_TO_TICKS((uint32_t)http_positive_or_default(cfg.conn_interval_sec, 600) * 1000U);

        if (cfg.post_url[0] && (last_post == 0 || (now - last_post) >= post_ticks)) {
            http_process_post(&cfg);
            last_post = xTaskGetTickCount();
        }
        if (cfg.config_url[0] && (last_cfg == 0 || (now - last_cfg) >= cfg_ticks)) {
            http_process_config_pull(&cfg);
            last_cfg = xTaskGetTickCount();
        }
        if (cfg.conn_url[0] && (last_conn == 0 || (now - last_conn) >= conn_ticks)) {
            http_process_connection_pull(&cfg);
            last_conn = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t http_client_init(const app_http_config_t *cfg, const http_client_callbacks_t *callbacks)
{
    if (!cfg || !callbacks || !callbacks->build_payload) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_callbacks = *callbacks;
    xSemaphoreGive(s_lock);

    storage_manager_init();
    ESP_LOGI(TAG, "HTTP client initialized: enabled=%d post=%s cfg=%s conn=%s",
             cfg->enabled,
             cfg->post_url,
             cfg->config_url,
             cfg->conn_url);
    return ESP_OK;
}

esp_err_t http_client_set_config(const app_http_config_t *cfg)
{
    if (!cfg || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "HTTP config updated: enabled=%d post_interval=%d cfg_interval=%d conn_interval=%d",
             cfg->enabled,
             cfg->post_interval_sec,
             cfg->config_interval_sec,
             cfg->conn_interval_sec);
    return ESP_OK;
}

esp_err_t http_client_start(void)
{
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(http_client_task,
                                "http_client_task",
                                HTTP_CLIENT_TASK_STACK,
                                NULL,
                                HTTP_CLIENT_TASK_PRIO,
                                &s_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool http_client_is_running(void)
{
    return s_task != NULL;
}
