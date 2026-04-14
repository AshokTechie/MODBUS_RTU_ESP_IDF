#include "app_config.h"
#include "azure_iot.h"
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modbus_rtu.h"
#include "nvs_flash.h"
#include "smartload_protocol.h"
#include "storage.h"
#include "wifi_mgr.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "main";

static modbus_rtu_handle_t s_modbus = NULL;
static smartload_protocol_t s_smartload = {0};
static app_runtime_config_t s_runtime_cfg = {0};

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
    s_runtime_cfg = *cfg;
    azure_iot_set_runtime_config(cfg);
    ESP_LOGI(TAG, "Twin updated telemetry_interval_sec=%d ro_code=%s",
             s_runtime_cfg.telemetry_interval_sec, s_runtime_cfg.ro_code);
}

static esp_err_t build_status_json(char *out, size_t out_size)
{
    smartload_status_t status = {0};
    ESP_RETURN_ON_ERROR(smartload_read_status(&s_smartload, &status), TAG, "status read failed");

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
        esp_err_t err = smartload_clear_status(&s_smartload);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "status cleared" : "clear failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_clear_alarms") == 0) {
        esp_err_t err = smartload_clear_alarms(&s_smartload);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "alarms cleared" : "clear failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_start_batch") == 0) {
        esp_err_t err = smartload_start_batch(&s_smartload);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "batch started" : "start failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_stop_batch") == 0) {
        esp_err_t err = smartload_stop_batch(&s_smartload);
        snprintf(response_json, response_json_size, "{\"response\":\"%s\"}", err == ESP_OK ? "batch stopped" : "stop failed");
        *status_code = err == ESP_OK ? 200 : 500;
        return err;
    }

    if (strcmp(method, "smartload_end_transaction") == 0) {
        esp_err_t err = smartload_end_transaction(&s_smartload);
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
        esp_err_t err = smartload_auth_batch(&s_smartload, preset_volume_x100, components);
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
        esp_err_t err = smartload_authorize(&s_smartload, recipe, additive, side_nozzle);
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
        int interval = s_runtime_cfg.telemetry_interval_sec > 0 ? s_runtime_cfg.telemetry_interval_sec : 30;
        if (build_status_json(payload, sizeof(payload)) == ESP_OK) {
            if (azure_iot_is_connected()) {
                azure_iot_publish_telemetry(payload);
            } else {
                ESP_LOGI(TAG, "%s", payload);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(storage_init());

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

    app_config_azure_t azure_cfg = {0};
    app_config_load_runtime(&s_runtime_cfg);

    if (app_config_load_azure(&azure_cfg) == ESP_OK && wifi_mgr_is_connected()) {
        ESP_ERROR_CHECK(azure_iot_init(&azure_cfg, &s_runtime_cfg));
        azure_iot_register_twin_callback(on_twin_update);
        azure_iot_register_method_callback(smartload_method_handler);
        if (azure_iot_connect() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            azure_iot_request_twin();
            azure_iot_start_background_task();
        }
    } else {
        ESP_LOGW(TAG, "Azure credentials missing or Wi-Fi offline, running local-only");
    }

    xTaskCreate(telemetry_task, "telemetry_task", 6144, NULL, 4, NULL);
}
