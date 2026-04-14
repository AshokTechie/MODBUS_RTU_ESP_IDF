#include "board.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "board";
static bool s_sd_mounted = false;
static sdmmc_card_t *s_sd_card = NULL;
static bool s_spi_bus_ready = false;

esp_err_t board_enable_rs485_power(bool enable)
{
    return gpio_set_level(BOARD_RS485_POWER_GPIO, enable ? 1 : 0);
}

esp_err_t board_set_rs485_shutdown(bool enable)
{
    return gpio_set_level(BOARD_RS485_SHDN_GPIO, enable ? 1 : 0);
}

esp_err_t board_set_rs485_tx_mode(bool enable)
{
    return gpio_set_level(BOARD_RS485_EN_GPIO, enable ? 1 : 0);
}

esp_err_t board_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_RS485_POWER_GPIO) |
                        (1ULL << BOARD_RS485_EN_GPIO) |
                        (1ULL << BOARD_RS485_SHDN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio_config failed");
    ESP_RETURN_ON_ERROR(board_enable_rs485_power(true), TAG, "5V enable failed");
    ESP_RETURN_ON_ERROR(board_set_rs485_shutdown(true), TAG, "shutdown pin failed");
    ESP_RETURN_ON_ERROR(board_set_rs485_tx_mode(false), TAG, "tx mode default failed");
    ESP_LOGI(TAG, "RS485 board pins initialized");
    return ESP_OK;
}

esp_err_t board_init_sd(void)
{
    if (!BOARD_SD_ENABLED) {
        ESP_LOGW(TAG, "SD card support is disabled in board.h");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_spi_bus_ready) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = BOARD_SD_MOSI_GPIO,
            .miso_io_num = BOARD_SD_MISO_GPIO,
            .sclk_io_num = BOARD_SD_SCLK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };

        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_spi_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = BOARD_SD_CS_GPIO;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted: %s", s_sd_card->cid.name);
    return ESP_OK;
}

bool board_sd_is_mounted(void)
{
    return s_sd_mounted;
}
