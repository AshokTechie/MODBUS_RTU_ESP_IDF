#include "storage_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "storage_manager";
static bool s_inited = false;
static unsigned long s_http_failure_seq = 0;

esp_err_t storage_manager_init(void)
{
    s_inited = true;
    return ESP_OK;
}

esp_err_t storage_manager_read_text(const char *path, char *buf, size_t buf_size, size_t *out_len)
{
    if (!s_inited) {
        storage_manager_init();
    }
    return storage_read(path, buf, buf_size, out_len);
}

esp_err_t storage_manager_write_text(const char *path, const char *data)
{
    if (!s_inited) {
        storage_manager_init();
    }

    if (!storage_can_write()) {
        size_t removed = 0;
        (void)storage_manager_rotate_logs(STORAGE_MANAGER_HTTP_PREFIX, STORAGE_MANAGER_HTTP_KEEP_COUNT, &removed);
        (void)storage_manager_rotate_logs(STORAGE_MANAGER_LOG_PREFIX, STORAGE_MANAGER_LOG_KEEP_COUNT, &removed);

        if (storage_can_write()) {
            return storage_write(path, data);
        }

        int usage = storage_usage_percent();
        ESP_LOGW(TAG, "Skipping write for %s: storage usage=%d%%", path ? path : "<null>", usage);
        return ESP_ERR_NO_MEM;
    }

    return storage_write(path, data);
}

esp_err_t storage_manager_remove(const char *path)
{
    if (!s_inited) {
        storage_manager_init();
    }
    return storage_remove(path);
}

esp_err_t storage_manager_rotate_logs(const char *prefix, size_t keep_count, size_t *removed_count)
{
    if (!s_inited) {
        storage_manager_init();
    }

    return storage_rotate_prefix(STORAGE_SPIFFS_MOUNT, prefix, keep_count, removed_count);
}

esp_err_t storage_manager_store_http_failure(const char *payload_json, char *out_path, size_t out_path_size)
{
    if (!payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_inited) {
        storage_manager_init();
    }

    size_t removed = 0;
    storage_manager_rotate_logs(STORAGE_MANAGER_HTTP_PREFIX, STORAGE_MANAGER_HTTP_KEEP_COUNT, &removed);

    char path[48];
    snprintf(path, sizeof(path), STORAGE_MANAGER_HTTP_PREFIX "%06lu.json", ++s_http_failure_seq);
    esp_err_t err = storage_manager_write_text(path, payload_json);
    if (err == ESP_OK) {
        if (out_path && out_path_size > 0) {
            snprintf(out_path, out_path_size, "%s", path);
        }
        ESP_LOGW(TAG, "Stored failed HTTP payload at %s", path);
    } else {
        ESP_LOGW(TAG, "Failed to store HTTP payload at %s: %s", path, esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_manager_get_stats(storage_manager_stats_t *out_stats)
{
    if (!out_stats) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    storage_spiffs_health_t health = storage_spiffs_health();
    out_stats->usage_percent = storage_usage_percent();
    out_stats->can_write = storage_can_write();
    out_stats->sd_available = storage_sd_is_available();
    out_stats->spiffs_total_bytes = health.total_bytes;
    out_stats->spiffs_used_bytes = health.used_bytes;
    return health.err;
}
