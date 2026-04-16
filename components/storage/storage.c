#include "storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "storage";
static SemaphoreHandle_t s_lock = NULL;
static bool s_sd_available = false;

static void build_full_path(char *out, size_t out_size, const char *mount, const char *path);

static int storage_compare_mtime(const void *lhs, const void *rhs)
{
    const storage_file_info_t *a = (const storage_file_info_t *)lhs;
    const storage_file_info_t *b = (const storage_file_info_t *)rhs;
    if (a->mtime < b->mtime) return -1;
    if (a->mtime > b->mtime) return 1;
    return strcmp(a->path, b->path);
}

static int storage_usage_percent_locked(void)
{
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) != ESP_OK || total == 0) {
        return -1;
    }
    return (int)((used * 100U) / total);
}

static size_t storage_collect_prefix_locked(const char *mount, const char *prefix,
                                            storage_file_info_t *files, size_t max_files)
{
    DIR *dir = opendir(mount);
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && count < max_files) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char logical_path[STORAGE_MAX_PATH_LEN];
        size_t entry_len = strlen(entry->d_name);
        if (entry_len + 2 > sizeof(logical_path)) {
            continue;
        }
        logical_path[0] = '/';
        memcpy(&logical_path[1], entry->d_name, entry_len);
        logical_path[entry_len + 1] = '\0';
        if (prefix && prefix[0] != '\0' && strncmp(logical_path, prefix, strlen(prefix)) != 0) {
            continue;
        }

        char full_path[STORAGE_MAX_PATH_LEN];
        build_full_path(full_path, sizeof(full_path), mount, logical_path);
        struct stat st = {0};
        if (stat(full_path, &st) != 0) {
            continue;
        }

        storage_file_info_t *item = &files[count++];
        snprintf(item->path, sizeof(item->path), "%s", logical_path);
        item->size_bytes = (size_t)st.st_size;
        item->mtime = (long)st.st_mtime;
    }

    closedir(dir);
    return count;
}

static void storage_try_cleanup_locked(void)
{
    storage_file_info_t files[16];
    size_t removed = 0;
    if (storage_usage_percent_locked() < STORAGE_USAGE_LIMIT_PCT) {
        return;
    }

    size_t count = storage_collect_prefix_locked(STORAGE_SPIFFS_MOUNT, "/http_", files, 16);
    if (count <= 8) {
        count = storage_collect_prefix_locked(STORAGE_SPIFFS_MOUNT, "/log_", files, 16);
    }
    if (count > 8) {
        qsort(files, count, sizeof(files[0]), storage_compare_mtime);
        for (size_t i = 0; i < count - 8; i++) {
            char full_path[STORAGE_MAX_PATH_LEN];
            build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, files[i].path);
            if (remove(full_path) == 0) {
                removed++;
            }
        }
    }

    if (removed > 0U) {
        ESP_LOGW(TAG, "Storage cleanup removed %u files to stay below %u%% usage",
                 (unsigned)removed, STORAGE_USAGE_LIMIT_PCT);
    }
}

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

    char full_path[STORAGE_MAX_PATH_LEN];
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
    FILE *f = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    char full_path[STORAGE_MAX_PATH_LEN];
    build_full_path(full_path, sizeof(full_path), STORAGE_SPIFFS_MOUNT, path);
    storage_try_cleanup_locked();
    int usage_pct = storage_usage_percent_locked();
    if (usage_pct >= STORAGE_USAGE_LIMIT_PCT) {
        ESP_LOGW(TAG, "Refusing SPIFFS write for %s: usage=%d%%", path, usage_pct);
        result = ESP_ERR_NO_MEM;
    } else {
        FILE *f = fopen(full_path, "w");
        if (f) {
            size_t len = strlen(data);
            result = fwrite(data, 1, len, f) == len ? ESP_OK : ESP_FAIL;
            fclose(f);
        }
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
    char full_path[STORAGE_MAX_PATH_LEN];
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
    char full_path[STORAGE_MAX_PATH_LEN];

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

int storage_usage_percent(void)
{
    int usage = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    usage = storage_usage_percent_locked();
    xSemaphoreGive(s_lock);
    return usage;
}

bool storage_can_write(void)
{
    int usage = storage_usage_percent();
    return usage < 0 || usage < STORAGE_USAGE_LIMIT_PCT;
}

esp_err_t storage_list_prefix(const char *mount, const char *prefix, storage_file_info_t *out_files,
                              size_t max_files, size_t *out_count)
{
    if (!mount || !out_files || !out_count || max_files == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    *out_count = storage_collect_prefix_locked(mount, prefix, out_files, max_files);
    xSemaphoreGive(s_lock);
    return *out_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_rotate_prefix(const char *mount, const char *prefix, size_t keep_count, size_t *removed_count)
{
    if (!mount || !prefix) {
        return ESP_ERR_INVALID_ARG;
    }

    if (removed_count) {
        *removed_count = 0;
    }

    storage_file_info_t files[16];
    size_t count = 0;
    esp_err_t err = storage_list_prefix(mount, prefix, files, sizeof(files) / sizeof(files[0]), &count);
    if (err != ESP_OK || count <= keep_count) {
        return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
    }

    qsort(files, count, sizeof(files[0]), storage_compare_mtime);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < count - keep_count; i++) {
        char full_path[STORAGE_MAX_PATH_LEN];
        build_full_path(full_path, sizeof(full_path), mount, files[i].path);
        if (remove(full_path) == 0 && removed_count) {
            (*removed_count)++;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
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
