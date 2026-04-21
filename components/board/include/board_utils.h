#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Read the chip identifier using the same efuse schema used by provisioning.
 *
 * Two schemas are supported by the first bit of eFuse BLK_KEY0:
 * - Legacy (bit=0): byte-swapped lower MAC bytes for backward compatibility.
 * - New    (bit=1): custom MAC if burned, otherwise default MAC packed BE.
 */
uint64_t board_read_chip_id(void);

/*
 * Return a single-letter board code used in telemetry payloads.
 * - "n": SmartLoad new PCB (HARDWARE_NEW_PCB)
 * - "u": Unknown or generic build
 */
const char *board_get_board_code(void);

/*
 * Probe the RTC over I2C and report whether it responds.
 * Uses RTC_ADDR from board_gpios.h.
 */
bool board_rtc_health(esp_err_t *err_out);
