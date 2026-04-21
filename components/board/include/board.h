#pragma once

#include "esp_err.h"
#include "board_gpios.h"
#include <stdbool.h>

/* Core board initialisation API */
esp_err_t board_init_i2c(void);
esp_err_t board_init_rs485(void);
esp_err_t board_init_sd(void);
esp_err_t board_enable_5v(bool enable);
esp_err_t board_init_sd_detect(void);
esp_err_t board_init_led(void);
esp_err_t board_init_all(void);
bool board_sd_is_mounted(void);

/* Compatibility API used by existing SmartLoad startup flow */
esp_err_t board_init(void);
esp_err_t board_enable_rs485_power(bool enable);
esp_err_t board_set_rs485_shutdown(bool enable);
esp_err_t board_set_rs485_tx_mode(bool enable);

/* Compatibility pin aliases for existing modules */
#define BOARD_RS485_POWER_GPIO      ((gpio_num_t)RS485_POWER_PIN)
#define BOARD_RS485_EN_GPIO         ((gpio_num_t)RS485_EN_PIN)
#define BOARD_RS485_RE_GPIO         ((gpio_num_t)RS485_RE_PIN)
#define BOARD_RS485_TX_GPIO         ((gpio_num_t)RS485_TX_PIN)
#define BOARD_RS485_RX_GPIO         ((gpio_num_t)RS485_RX_PIN)
#define BOARD_RS485_SHDN_GPIO       ((gpio_num_t)RS485_SE_PIN)

#define BOARD_SD_ENABLED            1
#define BOARD_SD_MOSI_GPIO          ((gpio_num_t)SD_MOSI_PIN)
#define BOARD_SD_MISO_GPIO          ((gpio_num_t)SD_MISO_PIN)
#define BOARD_SD_SCLK_GPIO          ((gpio_num_t)SD_SCLK_PIN)
#define BOARD_SD_CS_GPIO            ((gpio_num_t)SD_CS_PIN)

#define BOARD_I2C_SDA_GPIO          ((gpio_num_t)SDA_PIN)
#define BOARD_I2C_SCL_GPIO          ((gpio_num_t)SCL_PIN)
