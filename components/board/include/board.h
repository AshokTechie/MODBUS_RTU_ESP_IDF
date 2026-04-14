#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>

/*
 * Update these GPIOs to match your actual ESP32 board before flashing.
 * The RS485 pins below are copied from your Arduino SmartLoad project.
 */
#define BOARD_RS485_POWER_GPIO      GPIO_NUM_16
#define BOARD_RS485_EN_GPIO         GPIO_NUM_17
#define BOARD_RS485_TX_GPIO         GPIO_NUM_22
#define BOARD_RS485_RX_GPIO         GPIO_NUM_21
#define BOARD_RS485_SHDN_GPIO       GPIO_NUM_19

/*
 * SD card support is implemented but disabled by default because the Arduino
 * project did not expose the SD wiring. Set BOARD_SD_ENABLED to 1 and update
 * the pin map once your hardware is confirmed.
 */
#define BOARD_SD_ENABLED            1
#define BOARD_SD_MOSI_GPIO          GPIO_NUM_15
#define BOARD_SD_MISO_GPIO          GPIO_NUM_2
#define BOARD_SD_SCLK_GPIO          GPIO_NUM_14
#define BOARD_SD_CS_GPIO            GPIO_NUM_13

/*
 * RTC / I2C pins from your board definition.
 * These are not used by the current scaffold yet, but are kept here so the
 * board mapping stays in one place.
 */
#define BOARD_I2C_SDA_GPIO          GPIO_NUM_33
#define BOARD_I2C_SCL_GPIO          GPIO_NUM_32

esp_err_t board_init(void);
esp_err_t board_enable_rs485_power(bool enable);
esp_err_t board_set_rs485_shutdown(bool enable);
esp_err_t board_set_rs485_tx_mode(bool enable);
esp_err_t board_init_sd(void);
bool board_sd_is_mounted(void);
