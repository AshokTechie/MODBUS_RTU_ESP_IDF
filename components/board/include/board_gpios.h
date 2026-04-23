#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"

/*
 * Board-specific GPIO pin definitions.
 *
 * Exactly one HARDWARE_xxx macro is defined by the PlatformIO build
 * environment (-D flag in platformio.ini).
 */

#define SD_BUS_MODE_SPI         1
#define SD_BUS_MODE_SDMMC       2

/* ------------------------------------------------------------------
 * ESP32-DevKitC (esp32dev)
 * ------------------------------------------------------------------ */
#if defined(HARDWARE_ESP32DEV)

/* I2C - RTC DS3231 */
#define SDA_PIN                 33
#define SCL_PIN                 32
#define RTC_ADDR                0x68

/* RS485 transceiver */
#define RS485_TX_PIN            22
#define RS485_RX_PIN            21
#define RS485_EN_PIN            17
#define RS485_SE_PIN            19
#define HAS_RS485_SE            1
#define PIN_5V_EN               16
#define HAS_5V_EN               1
#define RS485_EN_ALWAYS_HIGH    0
#define RS485_USE_UART_RS485_MODE 0

/* SD card (SPI mode) */
#define SD_MISO_PIN             2
#define SD_MOSI_PIN             15
#define SD_SCLK_PIN             14
#define SD_CS_PIN               13
#define SD_BUS_MODE             SD_BUS_MODE_SPI
#define SD_BUS_WIDTH            1

/* Addressable LED */
#define WS2812_PIN              4
#define HAS_WS2812              1

/* UART port for RS485 */
#define RS485_UART_NUM          UART_NUM_2

/* LoRa - no M0/M1 on this board */
#define LORA_TX_PIN             5
#define LORA_RX_PIN             35
#define LORA_UART_NUM           UART_NUM_1

/* ------------------------------------------------------------------
 * Relcon board (ESP32-S3, 4 MB)
 * ------------------------------------------------------------------ */
#elif defined(HARDWARE_RELCON)

/* I2C - RTC */
#define SDA_PIN                 4
#define SCL_PIN                 5
#define RTC_ADDR                0x68

/* RS485 */
#define RS485_TX_PIN            18
#define RS485_RX_PIN            16
#define RS485_EN_PIN            17
#define RS485_USE_UART_RS485_MODE 1
#define RS485_EN_ALWAYS_HIGH    0

/* SD card (SPI mode) */
#define SD_MISO_PIN             36
#define SD_MOSI_PIN             38
#define SD_SCLK_PIN             37
#define SD_CS_PIN               39
#define SD_DETECT_PIN           41
#define HAS_SD_DETECT           1

/* SD card (SDMMC 4-bit mode - alternate) */
#define SD_CLK_PIN              37
#define SD_CMD_PIN              38
#define SD_DATA0_PIN            36
#define SD_DATA1_PIN            35
#define SD_DATA2_PIN            40
#define SD_DATA3_PIN            39
#define SD_BUS_MODE             SD_BUS_MODE_SDMMC
#define SD_BUS_WIDTH            4

/* Power control */
#define PIN_5V_EN               19
#define HAS_5V_EN               1

/* LoRa E220-900T22S */
#define LORA_TX_PIN             21
#define LORA_RX_PIN             14
#define LORA_M0                 48
#define LORA_M1                 47
#define LORA_UART_NUM           UART_NUM_1
#define HAS_LORA_M0M1           1

/* UART port for RS485 */
#define RS485_UART_NUM          UART_NUM_2

/* ------------------------------------------------------------------
 * Eviden board (ESP32-S3, 8 MB)
 * ------------------------------------------------------------------ */
#elif defined(HARDWARE_EVIDEN)

/* I2C - RTC */
#define SDA_PIN                 16
#define SCL_PIN                 15
#define RTC_ADDR                0x68

/* RS485 */
#define RS485_TX_PIN            36
#define RS485_RX_PIN            37
#define RS485_EN_PIN            38

/* SD card (SPI mode) */
#define SD_MISO_PIN             13
#define SD_MOSI_PIN             12
#define SD_SCLK_PIN             11
#define SD_CS_PIN               10
#define SD_BUS_MODE             SD_BUS_MODE_SPI

/* LoRa - no M0/M1, transparent mode */
#define LORA_TX_PIN             17
#define LORA_RX_PIN             18
#define LORA_UART_NUM           UART_NUM_1

/* UART port for RS485 */
#define RS485_UART_NUM          UART_NUM_2

/* ------------------------------------------------------------------
 * Orpack board (ESP32-S3, 16 MB)
 * ------------------------------------------------------------------ */
#elif defined(HARDWARE_ORPACK)

/* I2C - RTC */
#define SDA_PIN                 5
#define SCL_PIN                 4
#define RTC_ADDR                0x68

/* RS485 */
#define RS485_TX_PIN            44
#define RS485_RX_PIN            43
#define RS485_EN_PIN            45
#define RS485_USE_UART_RS485_MODE 1

/* SD card (SPI mode) */
#define SD_MISO_PIN             42
#define SD_MOSI_PIN             40
#define SD_SCLK_PIN             41
#define SD_CS_PIN               1
#define SD_BUS_MODE             SD_BUS_MODE_SPI

/* LoRa test-mode UART */
#define LORA_TX_PIN             17
#define LORA_RX_PIN             18
#define LORA_RESET_PIN          8
#define LORA_UART_NUM           UART_NUM_1
#define HAS_LORA_ORPACK_AT      1

/* UART port for RS485 */
#define RS485_UART_NUM          UART_NUM_2

/* ------------------------------------------------------------------
 * New PCB (ESP32-S3, 8 MB)
 * ------------------------------------------------------------------ */
#elif defined(HARDWARE_NEW_PCB)

/* I2C - RTC */
#define SDA_PIN                 8
#define SCL_PIN                 9
#define RTC_ADDR                0x68

/* RS485 - no EN pin on this board */
#define RS485_TX_PIN            17
#define RS485_RX_PIN            18

/* SD card (SPI mode) */
#define SD_MISO_PIN             13
#define SD_MOSI_PIN             11
#define SD_SCLK_PIN             12
#define SD_CS_PIN               10
#define SD_BUS_MODE             SD_BUS_MODE_SPI

/* LoRa E220-900T22S */
#define LORA_TX_PIN             5
#define LORA_RX_PIN             6
#define LORA_M0                 42
#define LORA_M1                 38
#define LORA_UART_NUM           UART_NUM_1
#define HAS_LORA_M0M1           1

/* UART port for RS485 */
#define RS485_UART_NUM          UART_NUM_2

#else
#error "No hardware variant defined. Add -DHARDWARE_XXX to platformio.ini build_flags."
#endif

/* ------------------------------------------------------------------
 * Defaults for optional features (when board does not define them)
 * ------------------------------------------------------------------ */
#ifndef RTC_ADDR
#define RTC_ADDR                0x68
#endif

#ifndef HAS_RS485_SE
#define HAS_RS485_SE            0
#endif

#ifndef RS485_EN_ALWAYS_HIGH
#define RS485_EN_ALWAYS_HIGH    0
#endif

#ifndef RS485_USE_UART_RS485_MODE
#define RS485_USE_UART_RS485_MODE 0
#endif

#ifndef HAS_5V_EN
#define HAS_5V_EN               0
#endif

#ifndef HAS_WS2812
#define HAS_WS2812              0
#endif

#ifndef HAS_SD_DETECT
#define HAS_SD_DETECT           0
#endif

#ifndef HAS_LORA_M0M1
#define HAS_LORA_M0M1           0
#endif

#ifndef HAS_LORA_ORPACK_AT
#define HAS_LORA_ORPACK_AT      0
#endif

#ifndef SD_BUS_MODE
#define SD_BUS_MODE             SD_BUS_MODE_SPI
#endif

#ifndef SD_BUS_WIDTH
#define SD_BUS_WIDTH            1
#endif

#ifndef RS485_EN_PIN
#define RS485_EN_PIN            (-1)
#endif

#ifndef RS485_RE_PIN
#define RS485_RE_PIN            RS485_EN_PIN
#endif

#ifndef RS485_SE_PIN
#define RS485_SE_PIN            (-1)
#endif

#ifndef PIN_5V_EN
#define PIN_5V_EN               (-1)
#endif

#ifndef RS485_POWER_PIN
#define RS485_POWER_PIN         PIN_5V_EN
#endif

/* ------------------------------------------------------------------
 * Debug mode overrides
 * ------------------------------------------------------------------ */
#ifdef DEBUG_MODE
#ifndef DEBUG_WIFI_SSID
#define DEBUG_WIFI_SSID         "ConnectSecure"
#endif
#ifndef DEBUG_WIFI_PASS
#define DEBUG_WIFI_PASS         "Con@Vjn#560040"
#endif
#ifndef DEBUG_AZURE_CONNSTR
#define DEBUG_AZURE_CONNSTR     ""
#endif
#endif

#ifndef DEFAULT_WIFI_SSID
#ifdef DEBUG_MODE
#define DEFAULT_WIFI_SSID       DEBUG_WIFI_SSID
#else
#define DEFAULT_WIFI_SSID       ""
#endif
#endif

#ifndef DEFAULT_WIFI_PASS
#ifdef DEBUG_MODE
#define DEFAULT_WIFI_PASS       DEBUG_WIFI_PASS
#else
#define DEFAULT_WIFI_PASS       ""
#endif
#endif

/* Common peripheral constants */
#define RS485_BAUD_RATE         9600
#define RS485_DATA_BITS         UART_DATA_8_BITS
#define RS485_PARITY            UART_PARITY_DISABLE
#define RS485_STOP_BITS         UART_STOP_BITS_1

#define I2C_MASTER_FREQ_HZ      100000
#define I2C_MASTER_PORT         I2C_NUM_0

#define ADU_DEVICE_MANUFACTURER "ESPRESSIF"
#define ADU_DEVICE_MODEL        "SMARTLOAD"
#define ADU_UPDATE_PROVIDER     "DigitalPetro"
#define ADU_UPDATE_NAME         "ESP32-Embedded"
#define ADU_DEVICE_VERSION      "2.1.1"

#define TELEMETRY_FREQ_MS       900000
