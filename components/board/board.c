#include "board.h"

#include <dirent.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "smartload_board";

static bool s_i2c_ready = false;
static bool s_rs485_uart_ready = false;
static bool s_sd_spi_bus_ready = false;
static bool s_sd_mounted = false;
static sdmmc_card_t *s_sd_card = NULL;

#ifndef SMARTLOAD_RS485_INIT_UART
#define SMARTLOAD_RS485_INIT_UART 1
#endif

static esp_err_t board_config_output_pin(int pin, int initial_level)
{
    if (pin < 0) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    return gpio_set_level((gpio_num_t)pin, initial_level);
}

static esp_err_t board_log_sd_mount_result(const char *mode)
{
    if (s_sd_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "SD card mounted via %s: %s, %llu MB",
             mode,
             s_sd_card->cid.name,
             (uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size / (1024 * 1024));

    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open /sdcard directory after mount");
        return ESP_OK;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGD(TAG, "SD root entry: %s", entry->d_name);
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t board_enable_5v(bool enable)
{
#if HAS_5V_EN
    return board_config_output_pin(RS485_POWER_PIN, enable ? 1 : 0);
#else
    (void)enable;
    return ESP_OK;
#endif
}

esp_err_t board_enable_rs485_power(bool enable)
{
    return board_enable_5v(enable);
}

esp_err_t board_set_rs485_shutdown(bool enable)
{
#if HAS_RS485_SE
    if (RS485_SE_PIN >= 0) {
        return gpio_set_level((gpio_num_t)RS485_SE_PIN, enable ? 1 : 0);
    }
#else
    (void)enable;
#endif

    return ESP_OK;
}

esp_err_t board_set_rs485_tx_mode(bool enable)
{
    if (RS485_EN_PIN >= 0) {
        return gpio_set_level((gpio_num_t)RS485_EN_PIN, enable ? 1 : 0);
    }

    (void)enable;
    return ESP_OK;
}

esp_err_t board_init_i2c(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "I2C init: SDA=%d SCL=%d freq=%d", SDA_PIN, SCL_PIN, I2C_MASTER_FREQ_HZ);

    const i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_MASTER_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "I2C driver already installed");
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_i2c_ready = true;
    return ESP_OK;
}

esp_err_t board_init_rs485(void)
{
    ESP_LOGI(TAG, "RS485 init starting...");

    const uart_config_t uart_cfg = {
        .baud_rate  = RS485_BAUD_RATE,
        .data_bits  = RS485_DATA_BITS,
        .parity     = RS485_PARITY,
        .stop_bits  = RS485_STOP_BITS,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG, "RS485 step 1: configuring UART params (baud=%d)...", RS485_BAUD_RATE);
    esp_err_t err = uart_param_config(RS485_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 UART param config FAILED: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS485 step 1: UART params OK");

    ESP_LOGI(TAG, "RS485 step 2: setting pins TX=%d RX=%d EN=%d...",
             RS485_TX_PIN, RS485_RX_PIN, RS485_EN_PIN);
    err = uart_set_pin(RS485_UART_NUM,
                       RS485_TX_PIN,
                       RS485_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 UART set pin FAILED: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS485 step 2: pins set OK");

#if SMARTLOAD_RS485_INIT_UART
    if (!s_rs485_uart_ready) {
        ESP_LOGI(TAG, "RS485 step 3: installing UART driver...");
        err = uart_driver_install(RS485_UART_NUM, 1024, 0, 0, NULL, 0);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "RS485 UART driver already installed; reusing existing driver");
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RS485 UART driver install FAILED: %s", esp_err_to_name(err));
            return err;
        }
        s_rs485_uart_ready = true;
        ESP_LOGI(TAG, "RS485 step 3: UART driver installed OK");
    }
#endif

#if RS485_EN_PIN >= 0
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << RS485_EN_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&en_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 EN gpio config FAILED: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(RS485_EN_PIN, RS485_EN_ALWAYS_HIGH ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 EN pin set FAILED: %s", esp_err_to_name(err));
        return err;
    }
#endif

#if HAS_RS485_SE
    ESP_LOGI(TAG, "RS485 step 4: setting SE pin %d HIGH (/SHDN active)...", RS485_SE_PIN);
    gpio_config_t se_cfg = {
        .pin_bit_mask = 1ULL << RS485_SE_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&se_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 SE gpio config FAILED: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(RS485_SE_PIN, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 SE pin set FAILED: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS485 step 4: SE pin HIGH OK");
#endif

    ESP_LOGI(TAG, "RS485 init COMPLETE: UART%d TX:%d RX:%d EN:%d SE:%d @ %d baud",
             RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN,
             RS485_EN_PIN,
#if HAS_RS485_SE
             RS485_SE_PIN,
#else
             -1,
#endif
             RS485_BAUD_RATE);

    return ESP_OK;
}

static esp_err_t board_init_sd_spi(void)
{
    if (!s_sd_spi_bus_ready) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_MOSI_PIN,
            .miso_io_num = SD_MISO_PIN,
            .sclk_io_num = SD_SCLK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };

        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "SPI bus already initialized");
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
            return err;
        }

        s_sd_spi_bus_ready = true;
    }

    const sdspi_device_config_t slot_cfg = {
        .host_id = SPI2_HOST,
        .gpio_cs = SD_CS_PIN,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = SDSPI_SLOT_NO_WP,
        .gpio_int = SDSPI_SLOT_NO_INT,
    };

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    const sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sd_mounted = true;
    return board_log_sd_mount_result("SPI");
}

esp_err_t board_init_sd(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

#if SD_BUS_MODE == SD_BUS_MODE_SPI
    return board_init_sd_spi();
#else
    ESP_LOGW(TAG, "SD bus mode %d is not supported in this build", SD_BUS_MODE);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t board_init_sd_detect(void)
{
#if HAS_SD_DETECT && defined(SD_DETECT_PIN)
    return board_config_output_pin(SD_DETECT_PIN, 0);
#else
    return ESP_OK;
#endif
}

esp_err_t board_init_led(void)
{
#if HAS_WS2812 && defined(WS2812_PIN)
    return board_config_output_pin(WS2812_PIN, 0);
#else
    return ESP_OK;
#endif
}

esp_err_t board_init_all(void)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Board init start");

    err = board_enable_5v(true);
    if (err != ESP_OK) {
        return err;
    }

    err = board_init_i2c();
    if (err != ESP_OK) {
        return err;
    }

    err = board_init_rs485();
    if (err != ESP_OK) {
        return err;
    }

    err = board_init_led();
    if (err != ESP_OK) {
        return err;
    }

    err = board_init_sd_detect();
    if (err != ESP_OK) {
        return err;
    }

    err = board_init_sd();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD init failed, continuing without SD: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Board init complete");
    return ESP_OK;
}

esp_err_t board_init(void)
{
    return board_init_all();
}

bool board_sd_is_mounted(void)
{
    return s_sd_mounted;
}
