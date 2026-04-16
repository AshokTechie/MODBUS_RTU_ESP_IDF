#include "smartload_http.h"

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_manager.h"
#include "storage_manager.h"
#include "wifi_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "smartload_http";

#define SMARTLOAD_HTTP_TASK_STACK 8192
#define SMARTLOAD_HTTP_TASK_PRIO 4
#define SMARTLOAD_HTTP_NS "smartload"
#define SMARTLOAD_HTTP_KEY_CONN "az_conn"
#define SMARTLOAD_HTTP_KEY_DEVICE_ID "http_did"

typedef struct {
    char *buf;
    size_t buf_size;
    size_t len;
    bool overflow;
} http_resp_ctx_t;

typedef struct {
    char payload[SMARTLOAD_HTTP_PAYLOAD_MAX];
} smartload_http_queue_item_t;

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static app_http_config_t s_cfg = {0};
static smartload_http_callbacks_t s_callbacks = {0};
static char s_device_id[APP_CONFIG_STR_LEN] = {0};
static bool s_conn_lookup_disabled = false;

static esp_err_t smartload_http_event_handler(esp_http_client_event_t *evt)
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

static void smartload_http_copy_config(app_http_config_t *out_cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_cfg = s_cfg;
    xSemaphoreGive(s_lock);
}

static void smartload_http_load_device_id_fallback(void)
{
    char nvs_device_id[APP_CONFIG_STR_LEN] = {0};
    if (nvs_manager_get_str(SMARTLOAD_HTTP_NS, SMARTLOAD_HTTP_KEY_DEVICE_ID,
                            nvs_device_id, sizeof(nvs_device_id)) == ESP_OK &&
        nvs_device_id[0] != '\0') {
        snprintf(s_device_id, sizeof(s_device_id), "%s", nvs_device_id);
        ESP_LOGI(TAG, "HTTP device_id fallback from NVS: %s", s_device_id);
        return;
    }
    /* IMPORTANT: HTTP must not fall back to Azure device_id. It must use the backend device_id
     * obtained from the device-details API (and persisted to NVS). */
    ESP_LOGW(TAG, "HTTP device_id fallback missing (NVS key '%s' unset)", SMARTLOAD_HTTP_KEY_DEVICE_ID);
}

static void smartload_http_store_device_id(const char *device_id)
{
    if (!device_id || device_id[0] == '\0') {
        return;
    }
    /* Backend device_id always wins for HTTP. */
    if (strcmp(s_device_id, device_id) != 0) {
        ESP_LOGI(TAG, "HTTP device_id set: %s", device_id);
    }
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
    (void)nvs_manager_set_str(SMARTLOAD_HTTP_NS, SMARTLOAD_HTTP_KEY_DEVICE_ID, s_device_id);
}

static int smartload_http_positive_or_default(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

static const char *smartload_http_trim_leading_ws(const char *text)
{
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static esp_err_t smartload_http_perform_json_request(esp_http_client_method_t method,
                                               const char *url,
                                               const char *payload,
                                               const char *device_id,
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
        .event_handler = smartload_http_event_handler,
        .user_data = &resp,
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
    if (device_id && device_id[0] != '\0') {
        esp_http_client_set_header(client, "device_id", device_id);
        ESP_LOGI(TAG, "HTTP header device_id=%s", device_id);
    } else {
        ESP_LOGW(TAG, "HTTP header device_id is empty (will likely fail auth)");
    }
    if (payload) {
        esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    }

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (resp.overflow) {
        ESP_LOGW(TAG, "Response from %s truncated at %u bytes", url, (unsigned)resp.len);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s (%s)", url, esp_err_to_name(err));
        return err;
    }
    if (*out_status == 404) {
        ESP_LOGW(TAG, "HTTP status=404 for %s body=%s", url, response_buf);
        return ESP_ERR_NOT_FOUND;
    }
    if (*out_status < 200 || *out_status >= 300) {
        ESP_LOGW(TAG, "HTTP status=%d for %s body=%s", *out_status, url, response_buf);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t smartload_http_request_with_retry(esp_http_client_method_t method,
                                             const char *url,
                                             const char *payload,
                                             const app_http_config_t *cfg,
                                             const char *device_id,
                                             char *response_buf,
                                             size_t response_buf_size,
                                             int *out_status)
{
    int retry_max = smartload_http_positive_or_default(cfg->retry_max, 3);
    int backoff_ms = smartload_http_positive_or_default(cfg->retry_backoff_ms, 2000);

    for (int attempt = 1; attempt <= retry_max; ++attempt) {
        if (!wifi_mgr_is_connected()) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "HTTP %s attempt %d/%d -> %s",
                 method == HTTP_METHOD_POST ? "POST" : "GET",
                 attempt,
                 retry_max,
                 url);
        esp_err_t err = smartload_http_perform_json_request(method, url, payload, device_id,
                                                            response_buf, response_buf_size, out_status);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP %s success status=%d <- %s body=%s",
                     method == HTTP_METHOD_POST ? "POST" : "GET",
                     *out_status,
                     url,
                     response_buf);
            return ESP_OK;
        }

        /* Do not retry on HTTP status failures like 401/404 (even if esp_http_client returned an err). */
        if (*out_status == 401) {
            ESP_LOGW(TAG, "HTTP status=401 for %s body=%s", url, response_buf);
            return ESP_FAIL;
        }
        if (*out_status == 404) {
            ESP_LOGW(TAG, "HTTP status=404 for %s body=%s", url, response_buf);
            return ESP_ERR_NOT_FOUND;
        }
        if (*out_status >= 400) {
            ESP_LOGW(TAG, "HTTP status=%d for %s body=%s", *out_status, url, response_buf);
            return ESP_FAIL;
        }

        /* Retry only for network/transport failures where we don't have a concrete HTTP status. */
        if (attempt < retry_max && *out_status == 0) {
            int delay_ms = backoff_ms * attempt;
            ESP_LOGW(TAG, "HTTP retry in %d ms for %s", delay_ms, url);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }
        return err != ESP_OK ? err : ESP_FAIL;
    }

    return ESP_FAIL;
}

static bool smartload_http_copy_json_string(cJSON *root, const char *key1, const char *key2, char *out, size_t out_size)
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

static esp_err_t smartload_http_apply_runtime_json(const char *body)
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

    smartload_http_copy_json_string(work, "device_name", "deviceName", next_cfg.device_name, sizeof(next_cfg.device_name));
    smartload_http_copy_json_string(work, "ro_code", "roCode", next_cfg.ro_code, sizeof(next_cfg.ro_code));

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
    return s_callbacks.apply_runtime_config(&next_cfg);
}

static esp_err_t smartload_http_parse_connection_payload(const char *body,
                                                   char *connection_string,
                                                   size_t connection_string_size,
                                                   app_config_azure_t *out_cfg,
                                                   char *resolved_device_id,
                                                   size_t resolved_device_id_size)
{
    if (!body || !connection_string || connection_string_size == 0 || !out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    connection_string[0] = '\0';
    if (resolved_device_id && resolved_device_id_size > 0) {
        resolved_device_id[0] = '\0';
    }

    const char *trimmed = smartload_http_trim_leading_ws(body);
    if (trimmed && strncmp(trimmed, "HostName=", 9) == 0) {
        snprintf(connection_string, connection_string_size, "%s", trimmed);
        ESP_RETURN_ON_ERROR(app_config_parse_connection_string(trimmed, out_cfg), TAG, "plain conn string parse failed");
        if (resolved_device_id && resolved_device_id_size > 0 && out_cfg->device_id[0] != '\0') {
            snprintf(resolved_device_id, resolved_device_id_size, "%s", out_cfg->device_id);
        }
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
        if (err == ESP_OK && resolved_device_id && resolved_device_id_size > 0) {
            snprintf(resolved_device_id, resolved_device_id_size, "%s", out_cfg->device_id);
        }
    } else {
        smartload_http_copy_json_string(root, "hostname", "HostName", out_cfg->hostname, sizeof(out_cfg->hostname));
        smartload_http_copy_json_string(root, "device_id", "DeviceId", out_cfg->device_id, sizeof(out_cfg->device_id));
        if (out_cfg->device_id[0] == '\0') {
            smartload_http_copy_json_string(root, "deviceId", NULL, out_cfg->device_id, sizeof(out_cfg->device_id));
        }
        if (resolved_device_id && resolved_device_id_size > 0 && out_cfg->device_id[0] != '\0') {
            snprintf(resolved_device_id, resolved_device_id_size, "%s", out_cfg->device_id);
        }
        smartload_http_copy_json_string(root, "shared_access_key", "SharedAccessKey",
                                  out_cfg->shared_access_key, sizeof(out_cfg->shared_access_key));
        if (out_cfg->shared_access_key[0] == '\0') {
            smartload_http_copy_json_string(root, "sharedAccessKey", NULL,
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

static void smartload_http_process_connection_pull(const app_http_config_t *cfg)
{
    if (!cfg->conn_url[0] || s_conn_lookup_disabled) {
        return;
    }

    /* Run once after Wi-Fi is connected (do not poll repeatedly). */
    s_conn_lookup_disabled = true;

    char response[768];
    char connection_string[APP_CONFIG_URL_LEN + APP_CONFIG_STR_LEN];
    char resolved_device_id[APP_CONFIG_STR_LEN];
    int status = 0;
    app_config_azure_t azure_cfg = {0};

    esp_err_t err = smartload_http_request_with_retry(HTTP_METHOD_GET,
                                                cfg->conn_url,
                                                NULL,
                                                cfg,
                                                s_device_id,
                                                response,
                                                sizeof(response),
                                                &status);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND || status == 404) {
            s_conn_lookup_disabled = true;
            ESP_LOGW(TAG, "Device-details lookup returned 404, keeping fallback device_id=%s",
                     s_device_id[0] ? s_device_id : "<unset>");
        }
        return;
    }

    err = smartload_http_parse_connection_payload(response,
                                            connection_string,
                                            sizeof(connection_string),
                                            &azure_cfg,
                                            resolved_device_id,
                                            sizeof(resolved_device_id));
    if (resolved_device_id[0] != '\0') {
        smartload_http_store_device_id(resolved_device_id);
        ESP_LOGI(TAG, "HTTP extracted backend device_id=%s", resolved_device_id);
    }
    if (err != ESP_OK || !app_config_validate_azure(&azure_cfg)) {
        if (resolved_device_id[0] != '\0') {
            ESP_LOGI(TAG, "Using resolved device_id=%s for HTTP", resolved_device_id);
        } else {
            ESP_LOGW(TAG, "Ignoring invalid HTTP connection payload");
        }
        return;
    }

    err = nvs_manager_set_str(SMARTLOAD_HTTP_NS, SMARTLOAD_HTTP_KEY_CONN, connection_string);
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

static void smartload_http_process_config_pull(const app_http_config_t *cfg)
{
    if (!cfg->config_url[0]) {
        return;
    }

    char response[768];
    int status = 0;
    esp_err_t err = smartload_http_request_with_retry(HTTP_METHOD_GET,
                                                cfg->config_url,
                                                NULL,
                                                cfg,
                                                s_device_id,
                                                response,
                                                sizeof(response),
                                                &status);
    if (err == ESP_OK) {
        err = smartload_http_apply_runtime_json(response);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP config apply failed: %s", esp_err_to_name(err));
        }
    }
}

static void smartload_http_process_queue_item(const app_http_config_t *cfg, const smartload_http_queue_item_t *item)
{
    if (!cfg->post_url[0] || !item || item->payload[0] == '\0') {
        return;
    }

    if (s_device_id[0] == '\0') {
        char saved_path[64];
        storage_manager_store_http_failure(item->payload, saved_path, sizeof(saved_path));
        ESP_LOGW(TAG, "HTTP POST skipped: backend device_id not resolved (queued %s)", saved_path);
        return;
    }

    char response[512];
    int status = 0;
    ESP_LOGI(TAG, "HTTP POST payload => %s", item->payload);
    ESP_LOGI(TAG, "HTTP POST using device_id=%s", s_device_id);
    esp_err_t err = smartload_http_request_with_retry(HTTP_METHOD_POST,
                                                cfg->post_url,
                                                item->payload,
                                                cfg,
                                                s_device_id,
                                                response,
                                                sizeof(response),
                                                &status);
    if (err != ESP_OK) {
        char saved_path[64];
        storage_manager_store_http_failure(item->payload, saved_path, sizeof(saved_path));
        ESP_LOGW(TAG, "HTTP POST failed for %s status=%d", cfg->post_url, status);
    }
}

static void smartload_http_task(void *arg)
{
    (void)arg;

    TickType_t last_cfg = 0;
    TickType_t last_conn = 0;

    for (;;) {
        app_http_config_t cfg;
        smartload_http_copy_config(&cfg);

        if (!cfg.enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!wifi_mgr_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t cfg_ticks = pdMS_TO_TICKS((uint32_t)smartload_http_positive_or_default(cfg.config_interval_sec, 300) * 1000U);
        TickType_t conn_ticks = pdMS_TO_TICKS((uint32_t)smartload_http_positive_or_default(cfg.conn_interval_sec, 600) * 1000U);

        if (cfg.conn_url[0] && (last_conn == 0 || (now - last_conn) >= conn_ticks)) {
            smartload_http_process_connection_pull(&cfg);
            last_conn = xTaskGetTickCount();
        }

        smartload_http_queue_item_t item = {0};
        while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
            smartload_http_process_queue_item(&cfg, &item);
        }

        if (cfg.config_url[0] && (last_cfg == 0 || (now - last_cfg) >= cfg_ticks)) {
            smartload_http_process_config_pull(&cfg);
            last_cfg = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t smartload_http_init(const app_http_config_t *cfg, const smartload_http_callbacks_t *callbacks)
{
    if (!cfg || !callbacks || !callbacks->apply_runtime_config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_queue) {
        s_queue = xQueueCreate(SMARTLOAD_HTTP_QUEUE_DEPTH, sizeof(smartload_http_queue_item_t));
        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_callbacks = *callbacks;
    xSemaphoreGive(s_lock);

    storage_manager_init();
    smartload_http_load_device_id_fallback();

    if (!s_task) {
        BaseType_t ok = xTaskCreate(smartload_http_task, "smartload_http_task", SMARTLOAD_HTTP_TASK_STACK, NULL, SMARTLOAD_HTTP_TASK_PRIO, &s_task);
        if (ok != pdPASS) {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "SmartLoad HTTP initialized: enabled=%d post=%s cfg=%s conn=%s",
             cfg->enabled,
             cfg->post_url,
             cfg->config_url,
             cfg->conn_url);
    return ESP_OK;
}

esp_err_t smartload_http_set_config(const app_http_config_t *cfg)
{
    if (!cfg || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t smartload_http_send(const char *payload_json)
{
    if (!payload_json || !s_queue) {
        return ESP_ERR_INVALID_ARG;
    }

    app_http_config_t cfg;
    smartload_http_copy_config(&cfg);
    if (!cfg.enabled || cfg.post_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    smartload_http_queue_item_t item = {0};
    snprintf(item.payload, sizeof(item.payload), "%s", payload_json);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        char saved_path[64];
        storage_manager_store_http_failure(payload_json, saved_path, sizeof(saved_path));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool smartload_http_is_running(void)
{
    return s_task != NULL;
}
