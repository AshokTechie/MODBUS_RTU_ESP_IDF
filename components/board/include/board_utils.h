#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Board-level utility functions.
 *
 * These live in the board component because they are hardware-specific
 * (chip ID, RTC health check) or board-identity functions that any
 * component may need to call.
 */

/**
 * Read the chip ID using the same schema as the provisioning service.
 *
 * Two schemas selected by the first bit of eFuse BLK_KEY0:
 *   Legacy (bit=0): byte-swapped lower 3 MAC bytes (Arduino-compat).
 *   New    (bit=1): custom MAC if burned, else factory MAC packed BE.
 */
uint64_t board_read_chip_id(void);

/**
 * Return a single-letter board code string for telemetry.
 * e.g. "n" = new_pcb, "o" = orpack, "r" = relcon, "e" = eviden,
 *      "l" = lilygo/esp32dev.
 */
const char *board_get_board_code(void);

/**
 * Probe the RTC over I2C and return whether it responds.
 * Uses the RTC_ADDR defined in board_gpios.h.
 * @param[out] err_out  Optional — set to the I2C error code.
 */
bool board_rtc_health(esp_err_t *err_out);