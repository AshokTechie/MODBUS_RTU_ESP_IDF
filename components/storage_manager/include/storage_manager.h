#pragma once

#include "esp_err.h"
#include "storage.h"

#include <stdbool.h>
#include <stddef.h>

#define STORAGE_MANAGER_HTTP_PREFIX "/http_"
#define STORAGE_MANAGER_HTTP_KEEP_COUNT 8
#define STORAGE_MANAGER_LOG_PREFIX "/log_"
#define STORAGE_MANAGER_LOG_KEEP_COUNT 8

typedef struct {
    int usage_percent;
    bool can_write;
    bool sd_available;
    size_t spiffs_total_bytes;
    size_t spiffs_used_bytes;
} storage_manager_stats_t;

esp_err_t storage_manager_init(void);
esp_err_t storage_manager_read_text(const char *path, char *buf, size_t buf_size, size_t *out_len);
esp_err_t storage_manager_write_text(const char *path, const char *data);
esp_err_t storage_manager_remove(const char *path);
esp_err_t storage_manager_rotate_logs(const char *prefix, size_t keep_count, size_t *removed_count);
esp_err_t storage_manager_store_http_failure(const char *payload_json, char *out_path, size_t out_path_size);
esp_err_t storage_manager_get_stats(storage_manager_stats_t *out_stats);
