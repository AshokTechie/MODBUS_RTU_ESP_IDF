#include "ota.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ota";
static bool s_running = false;

typedef struct {
    char url[512];
    char version[32];
} ota_args_t;

bool ota_parse_version(const char *str, int parts[4])
{
    if (!str || !str[0]) {
        return false;
    }
    parts[0] = parts[1] = parts[2] = parts[3] = 0;
    return sscanf(str, "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) > 0;
}

int ota_cmp_version(const int a[4], const int b[4])
{
    for (int i = 0; i < 4; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

void ota_get_current_version(char *buf, size_t buflen)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(buf, buflen, "%s", desc->version);
}

static void ota_task(void *arg)
{
    ota_args_t *args = (ota_args_t *)arg;
    esp_http_client_config_t http_cfg = {
        .url = args->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    free(args);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA complete, restarting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        s_running = false;
    }

    vTaskDelete(NULL);
}

bool ota_is_running(void)
{
    return s_running;
}

esp_err_t ota_start(const char *url, const char *target_version)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (target_version && target_version[0]) {
        char current[32];
        int cur[4];
        int tgt[4];
        ota_get_current_version(current, sizeof(current));
        if (ota_parse_version(current, cur) && ota_parse_version(target_version, tgt) &&
            ota_cmp_version(tgt, cur) <= 0) {
            return OTA_ERR_NOT_NEWER;
        }
    }

    ota_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(args->url, sizeof(args->url), "%s", url);
    snprintf(args->version, sizeof(args->version), "%s", target_version ? target_version : "");
    s_running = true;

    if (xTaskCreate(ota_task, "ota_task", 8192, args, 4, NULL) != pdPASS) {
        s_running = false;
        free(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
