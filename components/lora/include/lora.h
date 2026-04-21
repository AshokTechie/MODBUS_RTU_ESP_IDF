#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * LoRa driver for EBYTE E220 modules.
 *
 * On boards that expose M0/M1 pins:
 * - M0=0, M1=0: normal transparent mode
 * - M0=1, M1=1: configuration/AT mode
 */

esp_err_t lora_init(void);
bool lora_is_ok(void);
esp_err_t lora_send(const uint8_t *data, size_t len);
