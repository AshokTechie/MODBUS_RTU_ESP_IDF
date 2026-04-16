#include "app_config.h"
#include "azure_iot.h"
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modbus_rtu.h"
#include "nvs_flash.h"
#include "smartload_http.h"
#include "smartload_protocol.h"
#include "storage.h"
#include "storage_manager.h"
#include "wifi_mgr.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "main";

static modbus_rtu_handle_t s_modbus = NULL;
static smartload_protocol_t s_smartload = {0};
static app_runtime_config_t s_runtime_cfg = {0};
static app_http_config_t s_http_cfg = {0};
static SemaphoreHandle_t s_modbus_lock = NULL;
static SemaphoreHandle_t s_runtime_lock = NULL;

static esp_err_t connect_azure_if_possible(const app_config_azure_t *override_cfg);
static esp_err_t build_status_json(char *out, size_t out_size);

#define SMARTLOAD_HTTP_DEFAULT_POST_URL "https://shiftlogs.azurewebsites.net/api/atg_post"
#define SMARTLOAD_HTTP_DEFAULT_CONN_URL_FMT "https://enterpriseiotro.azurewebsites.net/api/getDeviceDetails?chipID=%llu"
#define SMARTLOAD_HTTP_PROVISION_CHIP_ID 9960508ULL

static void apply_http_defaults(void)
{
    if (s_http_cfg.post_url[0] == '\0') {
        snprintf(s_http_cfg.post_url, sizeof(s_http_cfg.post_url), "%s", SMARTLOAD_HTTP_DEFAULT_POST_URL);
    }
    if (s_http_cfg.conn_url[0] == '\0') {
        snprintf(s_http_cfg.conn_url,
                 sizeof(s_http_cfg.conn_url),
                 SMARTLOAD_HTTP_DEFAULT_CONN_URL_FMT,
                 (unsigned long long)SMARTLOAD_HTTP_PROVISION_CHIP_ID);
    }
    if (s_http_cfg.post_url[0] != '\0' || s_http_cfg.conn_url[0] != '\0' || s_http_cfg.config_url[0] != '\0') {
        s_http_cfg.enabled = true;
    }
}

static esp_err_t get_runtime_snapshot(app_runtime_config_t *out_cfg)
{
    if (!out_cfg || !s_runtime_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    *out_cfg = s_runtime_cfg;
    xSemaphoreGive(s_runtime_lock);
    return ESP_OK;
}

static esp_err_t apply_runtime_config(const app_runtime_config_t *cfg, const char *source)
{
    if (!cfg || !s_runtime_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    s_runtime_cfg = *cfg;
    xSemaphoreGive(s_runtime_lock);

    app_config_save_runtime(cfg);
    azure_iot_set_runtime_config(cfg);
    ESP_LOGI(TAG, "%s updated poll=%d telemetry=%d ro_code=%s device=%s",
             source ? source : "runtime",
             cfg->polling_interval_sec,
             cfg->telemetry_interval_sec,
             cfg->ro_code,
             cfg->device_name);
    return ESP_OK;
}

static esp_err_t apply_runtime_config_http(const app_runtime_config_t *cfg)
{
    return apply_runtime_config(cfg, "HTTP config");
}

static esp_err_t smartload_read_status_locked(smartload_status_t *out_status)
{
    if (!s_modbus_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_modbus_lock, portMAX_DELAY);
    esp_err_t err = smartload_read_status(&s_smartload, out_status);
    xSemaphoreGive(s_modbus_lock);
    return err;
}

static esp_err_t smartload_authorize_locked(uint16_t recipe, uint16_t additive_config, uint16_t side_nozzle)
{
    xSemaphoreTake(s_modbus_lock, portMAX_DELAY);
    esp_err_t err = smartload_authorize(&s_smartload, recipe, additive_config, side_nozzle);
    xSemaphoreGive(s_modbus_lock);
    return err;
}

static esp_err_t smartload_auth_batch_locked(uint32_t preset_volume_x100, uint16_t components)
{
    xSemaphoreTake(s_modbus_lock, portMAX_DELAY);
    esp_err_t err = smartload_auth_batch(&s_smartload, preset_volume_x100, components);
    xSemaphoreGive(s_modbus_lock);
    return err;
}

static esp_err_t smartload_send_simple_locked(esp_err_t (*fn)(smartload_protocol_t *))
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_modbus_lock, portMAX_DELAY);
    esp_err_t err = fn(&s_smartload);
    xSemaphoreGive(s_modbus_lock);
    return err;
}

static esp_err_t sync_time_via_ntp(void)
{
    if (time(NULL) > 1704067200) {
        return ESP_OK;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    esp_netif_sntp_deinit();
    return err;
}

static void on_twin_update(const app_runtime_config_t *cfg)
{
    apply_runtime_config(cfg, "Twin");
}

static void on_http_connection_update(const app_config_azure_t *cfg)
{
    ESP_LOGI(TAG, "HTTP connection string refreshed for device=%s", cfg ? cfg->device_id : "<null>");
    if (cfg && wifi_mgr_is_connected() && !azure_iot_is_connected()) {
        connect_azure_if_possible(cfg);
    }
}

static esp_err_t build_status_json(char *out, size_t out_size)
{
    smartload_status_t status = {0};
    ESP_RETURN_ON_ERROR(smartload_read_status_locked(&status), TAG, "status read failed");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msg_type", "smartload_status");
    cJSON_AddNumberToObject(root, "status_word", status.status_word);
    cJSON_AddNumberToObject(root, "delivery_volume_liters", smartload_volume_liters(status.delivery_volume_x100));
    cJSON_AddNumberToObject(root, "delivery_amount", smartload_amount(status.delivery_amount_x100));
    cJSON_AddNumberToObject(root, "unit_price", smartload_price(status.unit_price_x10000));
    cJSON_AddNumberToObject(root, "transaction_num", status.transaction_num);
    cJSON_AddNumberToObject(root, "temperature_c", smartload_temp_c(status.temperature_x100));
    cJSON_AddNumberToObject(root, "flow_rate_lpm", smartload_flow_lpm(status.flow_rate_x100));
    cJSON_AddNumberToObject(root, "hose_number", status.hose_number);
    cJSON_AddNumberToObject(root, "product_code", status.product_code);
    cJSON_AddNumberToObject(root, "alarm_register", status.alarm_register);

    if (!cJSON_PrintPreallocated(root, out, out_size, false)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t smartload_method_handler(const char *method,
                                          const char *payload_json,
                                          char *response_json,
                                          size_t response_json_size,
                                          int *status_code)
{
    if (!method || !response_json || !status_code) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(method, "smartload_read_status") == 0) {
        esp_err_t err = build_status_json(response_json, response_json_size);
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_clear_status") == 0) {
        esp_err_t err = smartload_send_simple_locked(smartload_clear_status);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "status cleared" : "clear failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_clear_alarms") == 0) {
        esp_err_t err = smartload_send_simple_locked(smartload_clear_alarms);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "alarms cleared" : "clear failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_start_batch") == 0) {
        esp_err_t err = smartload_send_simple_locked(smartload_start_batch);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "batch started" : "start failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_stop_batch") == 0) {
        esp_err_t err = smartload_send_simple_locked(smartload_stop_batch);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "batch stopped" : "stop failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_end_transaction") == 0) {
        esp_err_t err = smartload_send_simple_locked(smartload_end_transaction);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "transaction ended" : "end failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_auth_batch") == 0) {
        uint32_t preset_volume_x100 = 0;
        uint16_t components = 0;
        cJSON *root = cJSON_Parse(payload_json);
        if (root) {
            cJSON *pv = cJSON_GetObjectItem(root, "preset_volume_x100");
            cJSON *liters = cJSON_GetObjectItem(root, "preset_volume_liters");
            cJSON *comp = cJSON_GetObjectItem(root, "components");
            if (cJSON_IsNumber(pv)) {
                preset_volume_x100 = (uint32_t)pv->valuedouble;
            } else if (cJSON_IsNumber(liters)) {
                preset_volume_x100 = (uint32_t)(liters->valuedouble * 100.0 + 0.5);
            }
            if (cJSON_IsNumber(comp)) {
                components = (uint16_t)comp->valuedouble;
            }
            cJSON_Delete(root);
        }
        esp_err_t err = smartload_auth_batch_locked(preset_volume_x100, components);
        snprintf(response_json, response_json_size,
                 "{\"response\":\"%s\",\"preset_volume_x100\":%lu,\"components\":%u}",
                 err == ESP_OK ? "batch authorized" : "batch authorize failed",
                 (unsigned long)preset_volume_x100,
                 (unsigned)components);
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_authorize") == 0) {
        uint16_t recipe = 1;
        uint16_t additive = 0;
        uint16_t side_nozzle = 1;
        cJSON *root = cJSON_Parse(payload_json);
        if (root) {
            cJSON *r = cJSON_GetObjectItem(root, "recipe");
            cJSON *a = cJSON_GetObjectItem(root, "additive_config");
            cJSON *s = cJSON_GetObjectItem(root, "side_nozzle");
            if (cJSON_IsNumber(r)) recipe = (uint16_t)r->valuedouble;
            if (cJSON_IsNumber(a)) additive = (uint16_t)a->valuedouble;
            if (cJSON_IsNumber(s)) side_nozzle = (uint16_t)s->valuedouble;
            cJSON_Delete(root);
        }
        esp_err_t err = smartload_authorize_locked(recipe, additive, side_nozzle);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "authorized" : "authorize failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    return ESP_ERR_NOT_FOUND;
}

static void telemetry_task(void *arg)
{
    (void)arg;
    char payload[768];

    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        app_runtime_config_t runtime_cfg = {0};
        get_runtime_snapshot(&runtime_cfg);
        int interval = runtime_cfg.telemetry_interval_sec > 0 ? runtime_cfg.telemetry_interval_sec : 30;
        if (build_status_json(payload, sizeof(payload)) == ESP_OK) {
            ESP_LOGI(TAG, "%s", payload);
            if (azure_iot_is_connected() && azure_iot_publish_telemetry(payload) != ESP_OK) {
                ESP_LOGW(TAG, "Telemetry publish failed");
            }
            if (smartload_http_is_running() && smartload_http_send(payload) != ESP_OK) {
                ESP_LOGW(TAG, "SmartLoad HTTP send skipped or failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}

static esp_err_t connect_azure_if_possible(const app_config_azure_t *override_cfg)
{
    app_config_azure_t azure_cfg = {0};
    app_runtime_config_t runtime_cfg = {0};

    get_runtime_snapshot(&runtime_cfg);
    if (override_cfg) {
        azure_cfg = *override_cfg;
    } else {
        esp_err_t load_err = app_config_load_azure(&azure_cfg);
        if (load_err != ESP_OK) {
            ESP_LOGW(TAG, "Azure credentials load failed: %s", esp_err_to_name(load_err));
            return load_err;
        }
    }

    if (!app_config_validate_azure(&azure_cfg)) {
        ESP_LOGW(TAG, "Azure credentials are invalid or incomplete");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_mgr_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t init_err = azure_iot_init(&azure_cfg, &runtime_cfg);
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "Azure IoT init failed: %s", esp_err_to_name(init_err));
        return init_err;
    }

    azure_iot_register_twin_callback(on_twin_update);
    azure_iot_register_method_callback(smartload_method_handler);
    esp_err_t conn_err = azure_iot_connect();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "Azure IoT connect failed: %s", esp_err_to_name(conn_err));
        return conn_err;
    }

    azure_iot_start_background_task();
    ESP_LOGI(TAG, "Azure IoT connected and initialized");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "HTTP provisioning chip_id fixed=%llu", (unsigned long long)SMARTLOAD_HTTP_PROVISION_CHIP_ID);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(storage_manager_init());

    s_modbus_lock = xSemaphoreCreateMutex();
    s_runtime_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_modbus_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_runtime_lock ? ESP_OK : ESP_ERR_NO_MEM);

    if (board_init_sd() == ESP_OK) {
        storage_set_sd_available(board_sd_is_mounted());
    }

    ESP_ERROR_CHECK(wifi_mgr_init());
    if (wifi_mgr_connect() == ESP_OK) {
        sync_time_via_ntp();
    } else {
        ESP_LOGW(TAG, "Wi-Fi not connected, cloud features will stay offline");
    }

    modbus_rtu_config_t modbus_cfg = {
        .uart_num = UART_NUM_2,
        .tx_pin = BOARD_RS485_TX_GPIO,
        .rx_pin = BOARD_RS485_RX_GPIO,
        .de_pin = BOARD_RS485_EN_GPIO,
        .re_pin = GPIO_NUM_NC,
        .baudrate = 9600,
        .response_timeout_ms = 1000,
        .debug = true,
    };

    ESP_ERROR_CHECK(modbus_rtu_init(&modbus_cfg, &s_modbus));
    ESP_ERROR_CHECK(smartload_protocol_init(&s_smartload, s_modbus, 0x01));

    app_config_load_runtime(&s_runtime_cfg);

    app_config_load_http(&s_http_cfg);
    apply_http_defaults();
    smartload_http_callbacks_t http_callbacks = {
        .get_runtime_config = get_runtime_snapshot,
        .apply_runtime_config = apply_runtime_config_http,
        .on_connection_string = on_http_connection_update,
    };
    ESP_ERROR_CHECK(smartload_http_init(&s_http_cfg, &http_callbacks));
    if (!s_http_cfg.enabled) {
        ESP_LOGI(TAG, "SmartLoad HTTP disabled");
    }

    esp_err_t azure_err = connect_azure_if_possible(NULL);
    if (azure_err != ESP_OK) {
        if (azure_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Azure credentials missing, running local-only");
        } else if (!wifi_mgr_is_connected()) {
            ESP_LOGW(TAG, "Wi-Fi offline, Azure IoT features unavailable");
        }
    }

    xTaskCreate(telemetry_task, "telemetry_task", 6144, NULL, 4, NULL);
}
