#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define OTA_ERR_NOT_NEWER 0x1301

bool ota_is_running(void);
bool ota_parse_version(const char *str, int parts[4]);
int ota_cmp_version(const int a[4], const int b[4]);
void ota_get_current_version(char *buf, size_t buflen);
esp_err_t ota_start(const char *url, const char *target_version);
