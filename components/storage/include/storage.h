#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_SPIFFS_MOUNT "/spiffs"
#define STORAGE_SD_MOUNT     "/sdcard"

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    esp_err_t err;
} storage_spiffs_health_t;

typedef struct {
    bool write_ok;
    bool read_ok;
    bool match_ok;
    esp_err_t err;
} storage_sd_health_t;

esp_err_t storage_init(void);
void storage_set_sd_available(bool available);
bool storage_sd_is_available(void);
esp_err_t storage_read(const char *path, char *buf, size_t buf_size, size_t *out_len);
esp_err_t storage_write(const char *path, const char *data);
esp_err_t storage_remove(const char *path);
bool storage_exists(const char *path);
storage_spiffs_health_t storage_spiffs_health(void);
storage_sd_health_t storage_sd_health(void);
