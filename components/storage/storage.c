#include "storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "storage";
static SemaphoreHandle_t s_lock = NULL;
static bool s_sd_available = false;

static void build_full_path(char *out, size_t out_size, const char *mount, const char *path)
{
    snprintf(out, out_size, "%s%s", mount, path);
}

esp_err_t storage_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_spiffs_conf_t cfg = {
        .base_path = STORAGE_SPIFFS_MOUNT,
        .partition_label = "spiffs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: total=%uKB used=%uKB", (unsigned)(total / 1024), (unsigned)(used / 1024));
    return ESP_OK;
}

void storage_set_sd_available(bool available)
{
    s_sd_available = available;
}

bool storage_sd_is_available(void)
{
    return s_sd_available;
}

esp_err_t storage_read(const char *path, char *buf, size_t buf_size, size_t *out_len)
{
    if (!path || !buf || buf_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    char full_path[96];
    FILE *f = NULL;
    build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, path);
    f = fopen(full_path, "r");

    if (!f && s_sd_available) {
        build_full_path(full_path, sizeof(full_path), STORAGE_SD_MOUNT, path);
        f = fopen(full_path, "r");
    }

    if (!f) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t read = fread(buf, 1, buf_size - 1, f);
    fclose(f);
    buf[read] = '\0';
    if (out_len) {
        *out_len = read;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t storage_write(const char *path, const char *data)
{
    if (!path || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_FAIL;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    char full_path[96];
    build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, path);
    FILE *f = fopen(full_path, "w");
    if (f) {
        size_t len = strlen(data);
        result = fwrite(data, 1, len, f) == len ? ESP_OK : ESP_FAIL;
        fclose(f);
    }

    if (s_sd_available) {
        build_full_path(full_path, sizeof(full_path), STORAGE_SD_MOUNT, path);
        f = fopen(full_path, "w");
        if (f) {
            fwrite(data, 1, strlen(data), f);
            fclose(f);
        }
    }

    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t storage_remove(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char full_path[96];
    build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, path);
    remove(full_path);
    if (s_sd_available) {
        build_full_path(full_path, sizeof(full_path), STORAGE_SD_MOUNT, path);
        remove(full_path);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool storage_exists(const char *path)
{
    struct stat st;
    char full_path[96];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, path);
    bool found = stat(full_path, &st) == 0;
    if (!found && s_sd_available) {
        build_full_path(full_path, sizeof(full_path), STORAGE_SD_MOUNT, path);
        found = stat(full_path, &st) == 0;
    }
    xSemaphoreGive(s_lock);
    return found;
}

storage_spiffs_health_t storage_spiffs_health(void)
{
    storage_spiffs_health_t health = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    health.err = esp_spiffs_info("spiffs", &health.total_bytes, &health.used_bytes);
    xSemaphoreGive(s_lock);
    return health;
}

storage_sd_health_t storage_sd_health(void)
{
    storage_sd_health_t health = {0};
    if (!s_sd_available) {
        health.err = ESP_ERR_INVALID_STATE;
        return health;
    }

    const char *probe = STORAGE_SD_MOUNT "/.health_probe";
    const char *payload = "health_ok";
    char buf[16] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(probe, "w");
    if (!f) {
        xSemaphoreGive(s_lock);
        health.err = ESP_FAIL;
        return health;
    }
    health.write_ok = fputs(payload, f) != EOF;
    fclose(f);

    f = fopen(probe, "r");
    if (f) {
        health.read_ok = fgets(buf, sizeof(buf), f) != NULL;
        fclose(f);
    }
    remove(probe);
    xSemaphoreGive(s_lock);

    health.match_ok = strcmp(buf, payload) == 0;
    health.err = (health.write_ok && health.read_ok && health.match_ok) ? ESP_OK : ESP_FAIL;
    return health;
}
